#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/i2s_std.h"

static i2s_chan_handle_t   tx_chan;    //I2S tx channel handlers
#define MAXBYTES 64 //longest frame in bytes (might be more, TBD)
#define N 10 //amount of uint32_t needed for one serial bit
#define BUFF_SIZE   MAXBYTES*11*N //units of uint32 with 11 bits per serial-byte-transmission 8E1
#define ONE  0xFFFFFFFF
#define ZERO 0x00000000
#define SETMARK(i)   do {for (int j=0; j<N; j++) dma_buf[i*N+j]=ONE; \
                        even++;} while(0)
#define SETSPACE(i)  do {for (int j=0; j<N; j++) dma_buf[i*N+j]=ZERO; \
                        } while(0)
void send_frame(uint8_t *payload, int len) {
    if (len>MAXBYTES) {printf("frame too long\n"); return;}
    int even,byte,bit=0; //bit keeps track of bit-representation in dma_buf over an entire frame
    size_t bytes_loaded;
    uint32_t dma_buf[BUFF_SIZE]={0};
    
    for (byte=0 ; byte<len ; byte++) { //iterate bytes
        printf("S--0x%02x--ps ",payload[byte]);
        even=0; //https://en.wikipedia.org/wiki/RS-232 for details
        SETMARK(bit); bit++; //start bit
        for (int i=0; i<8; i++) { //iterate payload byte bits
            if (payload[byte]&(1<<i)) SETSPACE(bit); else SETMARK(bit);
            bit++;
        }
        if (even%2) SETMARK(bit); else SETSPACE(bit); bit++;//parity bit
        SETSPACE(bit); bit++; //stop bit
    }
    printf("\n");
    for (int i=0; i<bit; i++) printf("%d%s",dma_buf[i*N]?1:0,i%11==10?" ":"");
    //transmit the dma_buf once
    if (i2s_channel_preload_data(tx_chan, dma_buf, BUFF_SIZE, &bytes_loaded)!=ESP_OK) printf("i2s_channel_preload_data failed\n");
    printf("%d bytes preloaded\n",bytes_loaded);
    if (i2s_channel_enable(tx_chan)!=ESP_OK) printf("i2s_channel_enable failed\n"); //Enable the TX channel
    vTaskDelay(30*len/portTICK_PERIOD_MS); //each byte-transmission is 18.33 ms, message is len*18.33 ms but somehow need a bit more
    if (i2s_channel_disable(tx_chan)!=ESP_OK) printf("i2s_channel_disable failed\n"); //Disable the TX channel
}


void send_init() { //note that idle voltage is zero and cannot be flipped
    i2s_chan_info_t tx_chan_info;
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(3000), // min value higher than 2000 so use 3000, not 1500
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk=I2S_GPIO_UNUSED, .bclk=I2S_GPIO_UNUSED, .ws=I2S_GPIO_UNUSED, .din=I2S_GPIO_UNUSED, },
    };
    if (i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL)!=ESP_OK) printf("i2s_new_channel failed\n"); //no Rx
    tx_std_cfg.gpio_cfg.dout=GPIO_NUM_13; //closest to +5V pin on ESP32 board so can use 6 pin header
    if (i2s_channel_init_std_mode(tx_chan, &tx_std_cfg)!=ESP_OK) printf("i2s_channel_init_std_mode failed\n");
    i2s_channel_get_info(tx_chan, &tx_chan_info);
    printf("dma_buff_size assigned=%ld\n",tx_chan_info.total_dma_buf_size);
}

void main_task(void *arg) {
    uint8_t payloadC[]={0x02,0x05,0x23,0x46,0x54,0x0d,0x31,0x39,0x0d,0x02,0x03,0x3f};
    uint8_t payload2[]={0x02,0x05,0x22,0x33,0x56,0x0d,0x32,0x0d,0x02,0x03,0x73};
    uint8_t payload7[]={0x02,0x05,0x22,0x33,0x56,0x0d,0x37,0x0d,0x02,0x03,0x76};
    printf("buff_size=%d\n", BUFF_SIZE);
    send_init();
    vTaskDelay(100);
    send_frame(payloadC,sizeof(payloadC)); //test start cyclic pattern
    while (1) {
        vTaskDelay(400);
        send_frame(payload2,sizeof(payload2)); //level 2
        vTaskDelay(400);
        send_frame(payload7,sizeof(payload7)); //level 7
    }
}

void app_main(void) {
    xTaskCreatePinnedToCore(main_task,"main",65536,NULL,1,NULL,0); //make a huge stack, else issue with memory
    while (true) {
        vTaskDelay(1000); 
    }
}
