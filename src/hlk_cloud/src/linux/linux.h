#ifndef __HLK_LINUX_H__
#define __HLK_LINUX_H__


extern int hi_link_set_channel_value_linux(char *channel, int value);
extern int hi_link_get_channel_value_linux(char *channel, int *value);

extern void mt7628_upgrade_firmware(void);

#endif
