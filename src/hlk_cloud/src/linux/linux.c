#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include "hi_link.h"
#include "hi_mqtt.h"
#include "hi_link_ipc.h"
#include "app_api.h"
#include "hlk_log.h"


typedef struct {
	int gpio;				//gpio number (0 ~ 23)
	unsigned int on;		//interval of led on
	unsigned int off;		//interval of led off
	unsigned int blinks;	//number of blinking cycles
	unsigned int rests;		//number of break cycles
	unsigned int times;		//blinking times
} ralink_gpio_led_info;

#define GPIO_DEV	"/dev/gpio"
#define RALINK_GPIO_LED_SET		0x41

void led_blink(int times)
{
	int led_fd;
	ralink_gpio_led_info led;
	
	led.gpio = 44;
	led.on = 6;
	led.off = 2;
	led.blinks = 3;
	led.rests = 1;
	led.times = times;

	system("reg s 0; reg w 64 0x1;");

	led_fd = open(GPIO_DEV, O_RDONLY);
	
	ioctl(led_fd, RALINK_GPIO_LED_SET, &led);
}


int hi_link_set_channel_value_linux(char *channel, int value)
{
    HLK_LOG_INFO("hi_link_set_channel_value_linux channel: %s, value: %d\r\n", channel, value);
    return 0;
}


int hi_link_get_channel_value_linux(char *channel, int *value)
{
    HLK_LOG_INFO("hi_link_get_channel_value_linux channel: %s\r\n", channel);
    return 0;
}


inline int mtd_write_firmware(char *filename)
{
	char cmd[512];
	int status;
    led_blink(10000);

    snprintf(cmd, sizeof(cmd), "/bin/mtd_write write %s Kernel", filename);
    status = SYSTEM(cmd);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;

    return 0;
}



void mt7628_upgrade_firmware(void)
{
    if (access(SYSUPGRADE_BIN_PATH_TMP, F_OK) == 0) {
        SYSTEM("touch /tmp/is_upgrade_ing");
        if (mtd_write_firmware(SYSUPGRADE_BIN_PATH_TMP)) {
            HLK_LOG_ERR("mt7628_upgrade_firmware mtd_write_firmware failed\r\n");
            return;
        }
    } else {
        HLK_LOG_ERR("mt7628_upgrade_firmware file not found\r\n");
    }
}
