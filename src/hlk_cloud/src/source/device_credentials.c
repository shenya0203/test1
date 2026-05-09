#include <sys/types.h>
#include <syslog.h>
#ifdef HLK_PRODUCT_RM65
#include "rm65.h"
#elif HLK_PRODUCT_RM68
#include "rm68.h"
#elif HLK_PRODUCT_RM60
#include "rm60.h"
#elif HLK_PRODUCT_RM50
#include "rm50.h"
#elif HLK_PRODUCT_7628
#include "7628.h"
#endif


/**
 * @brief 从配置中读取许可证信息(五元组信息)
 * @param DN_   [out] DeviceName
 * @param PjK_  [out] ProjectKey
 * @param PdK_  [out] ProductKey
 * @param PdS_  [out] ProductSecret
 * @param DS_   [out] DeviceSecret
 * @param size_  [in] 数组大小
 * @return 0成功，-1失败
 */
int cfmGetLicense(char *DN_,char *PjK_,char *PdK_,char *PdS_,char *DS_,  size_t size_)
{
    int ret = 0;

    if (!DN_ || !PjK_ || !PdK_ || !PdS_ || !DS_) {
        syslog(LOG_DEBUG, "Err input value is NULL\r\n");
        return -1;
    }
    #ifdef HLK_PRODUCT_RM65
    ret = cfmGetLicense_RM65(DN_, PjK_, PdK_, PdS_, DS_, size_);
    #elif HLK_PRODUCT_RM68
    ret = cfmGetLicense_RM68(DN_, PjK_, PdK_, PdS_, DS_, size_);
    #elif HLK_PRODUCT_RM60
    ret = cfmGetLicense_RM60(DN_, PjK_, PdK_, PdS_, DS_, size_);
    #elif HLK_PRODUCT_RM50
    ret = cfmGetLicense_RM50(DN_, PjK_, PdK_, PdS_, DS_, size_);
    #elif HLK_PRODUCT_7628
    ret = cfmGetLicense_mt7628(DN_, PjK_, PdK_, PdS_, DS_, size_);
    #endif

exit_:
    return ret;
}

/**
 * @brief 设置许可证信息(五元组信息)到配置中
 * @param DN_   [in] DeviceName
 * @param PjK_  [in] ProjectKey
 * @param PdK_  [in] ProductKey
 * @param PdS_  [in] ProductSecret
 * @param DS_   [in] DeviceSecret
 * @return 0成功，-1失败
 */
int cfmSetLicense(const char *DN_, const char *PjK_, const char *PdK_, const char *PdS_, const char *DS_)
{
    int ret = 0;

    if (!DN_ || !PjK_ || !PdK_ || !PdS_ || !DS_) {
        syslog(LOG_DEBUG, "Err input value is NULL\r\n");
        return -1;
    }
    
    #ifdef HLK_PRODUCT_RM65
    ret = cfmSetLicense_RM65(DN_, PjK_, PdK_, PdS_, DS_);
    #elif HLK_PRODUCT_RM68
    ret = cfmSetLicense_RM68(DN_, PjK_, PdK_, PdS_, DS_);
    #elif HLK_PRODUCT_RM60
    ret = cfmSetLicense_RM60(DN_, PjK_, PdK_, PdS_, DS_);
    #elif HLK_PRODUCT_RM50
    ret = cfmSetLicense_RM50(DN_, PjK_, PdK_, PdS_, DS_);
    #elif HLK_PRODUCT_7628
    ret = cfmSetLicense_mt7628(DN_, PjK_, PdK_, PdS_, DS_);
    #endif

    return ret;
}

int cfmClearLicense(void)
{
    int ret = 0;

    #ifdef HLK_PRODUCT_RM65
    ret = cfmClearLicense_RM65();
    #elif HLK_PRODUCT_RM68
    ret = cfmClearLicense_RM68();
    #elif HLK_PRODUCT_RM60
    ret = cfmClearLicense_RM60();
    #elif HLK_PRODUCT_RM50
    ret = cfmClearLicense_RM50();
    #elif HLK_PRODUCT_7628
    ret = cfmClearLicense_mt7628();
    #endif

    return ret;
}