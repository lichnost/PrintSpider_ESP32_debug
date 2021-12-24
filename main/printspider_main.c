#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"

#include "printspider_i2s.h"
#include "printspider_buffer_filler.h"
#include "printspider_genwaveform.h"

static const char *TAG = "PritSpider";

static bool image_color = true;

//GPIO numbers for the lines that are connected (via level converters) to the printer cartridge.
#define PIN_NUM_CART_S1 4
#define PIN_NUM_CART_S2 5
#define PIN_NUM_CART_S3 18
#define PIN_NUM_CART_S4 19
#define PIN_NUM_CART_S5 21
#define PIN_NUM_CART_CSYNC 22
#define PIN_NUM_CART_D1 32
#define PIN_NUM_CART_D2 33
#define PIN_NUM_CART_D3 25
#define PIN_NUM_CART_DCLK 23
#define PIN_NUM_CART_F3 26
#define PIN_NUM_CART_F5 27

//Queue for nozzle data
QueueHandle_t nozdata_queue;

#define WAVEFORM_DMALEN 1500


//Selecting printsider waveform.
static esp_err_t select_waveform() {
    if (image_color) {
        ESP_LOGI(TAG, "Setting waveform PRINTSPIDER_WAVEFORM_COLOR_B");
    	printspider_select_waveform(PRINTSPIDER_WAVEFORM_COLOR_B);
    } else {
        ESP_LOGI(TAG, "Setting waveform PRINTSPIDER_WAVEFORM_BLACK_B");
        printspider_select_waveform(PRINTSPIDER_WAVEFORM_BLACK_B);
    }
    return ESP_OK;
}

static esp_err_t init_printing() {

    //Create nozzle data queue
	nozdata_queue=xQueueCreate(1, PRINTSPIDER_NOZDATA_SZ);

    //Initialize I2S parallel device. Use the function to generate waveform data from nozzle data as the callback
	//function.
	i2s_parallel_config_t i2scfg={
		.gpio_bus={
			PIN_NUM_CART_D1, //0
			PIN_NUM_CART_D2, //1
			PIN_NUM_CART_D3, //2
			PIN_NUM_CART_CSYNC, //3
			PIN_NUM_CART_S2, //4
			PIN_NUM_CART_S4, //5
			PIN_NUM_CART_S1, //6
			PIN_NUM_CART_S5, //7
			PIN_NUM_CART_DCLK, //8
			PIN_NUM_CART_S3, //9
			PIN_NUM_CART_F3, //10
			PIN_NUM_CART_F5, //11
			-1, -1, -1, -1 //12-15 - unused
		},
		.bits=I2S_PARALLEL_BITS_16,
		.clkspeed_hz=8000000, //8000000, //8MHz //1000000, //1MHz //3333333, //3.3MHz
		.bufsz=WAVEFORM_DMALEN*sizeof(uint16_t),
		.refill_cb=printspider_buffer_filler_fn,
		.refill_cb_arg=nozdata_queue
	};

    ESP_LOGI(TAG, "Setting up parallel I2S bus at I2S%d", 1);
    i2s_parallel_setup(&I2S1, &i2scfg);
	i2s_parallel_start(&I2S1);

    select_waveform();

    return ESP_OK;
}

//constant pixel
uint8_t image_get_pixel(int x, int y, int color) {
	return 0x7f;
}

void send_image_row_color(int pos) {
	uint8_t nozdata[PRINTSPIDER_NOZDATA_SZ];
	memset(nozdata, 0, PRINTSPIDER_NOZDATA_SZ);
	for (int c=0; c<3; c++) {
		for (int y=0; y<PRINTSPIDER_COLOR_NOZZLES_IN_ROW; y++) {
            //Instead of y, there was an y*2 in the original implementation of the next line in spritestm's code,
            //because of image height is 168 and color cartridge has a twice smaller number of nozzles - 84.
            //It needed to print image at full height.
			uint8_t v=image_get_pixel(pos-c*PRINTSPIDER_COLOR_ROW_OFFSET, y, c);
			//Note the v returned is 0 for black, 255 for the color. We need to invert that here as we're printing on
			//white.
			v=255-v;
			//Random-dither. The chance of the nozzle firing is equal to (v/256).
			if (v>(rand()&255)) {
				//Note: The actual nozzles for the color cart start around y=14
				printspider_fire_nozzle_color(nozdata, y+PRINTSPIDER_COLOR_VERTICAL_OFFSET, c);
			}
		}
	}
	//Send nozzle data to queue so ISR can pick up on it.
	xQueueSend(nozdata_queue, nozdata, portMAX_DELAY);
}

void send_image_row_black(int pos) {
	uint8_t nozdata[PRINTSPIDER_NOZDATA_SZ];
	memset(nozdata, 0, PRINTSPIDER_NOZDATA_SZ);
	for (int row=0; row<2; row++) {
		for (int y=0; y<PRINTSPIDER_BLACK_NOZZLES_IN_ROW; y++) {
			//We take anything but white in any color channel of the image to mean we want black there.
			if (image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 0)!=0xff ||
				image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 1)!=0xff ||
				image_get_pixel(pos+row*PRINTSPIDER_BLACK_ROW_OFFSET, y, 2)!=0xff) {
				//Random-dither 50%, as firing all nozzles is a bit hard on the power supply.
				if (rand()&1) {
					printspider_fire_nozzle_black(nozdata, y, row);
				}
			}
		}
	}
	//Send nozzle data to queue so ISR can pick up on it.
	xQueueSend(nozdata_queue, nozdata, portMAX_DELAY);
}

void print_loop() {
	while(true) {
        vTaskDelay(3000/portTICK_PERIOD_MS);
        ESP_LOGI(TAG, "Printing...");
        if (image_color) {
            send_image_row_color(0);
        } else {
            send_image_row_black(0);
        }
        ESP_LOGI(TAG, "Print done");
    }
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "init_printing");
    init_printing();

	//As the printcart interrupt is on core 0, better use core 1 for the image processing stuff that happens in the main loop.
	xTaskCreatePinnedToCore(print_loop, "print_loop", 1024*16, NULL, 1, NULL, 1);
	// print_loop();
}
