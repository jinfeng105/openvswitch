#ifndef __LINUX_NETDEV_FEATURES_WRAPPER_H
#define __LINUX_NETDEV_FEATURES_WRAPPER_H

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
#include_next <linux/netdev_features.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,10,0)
#define NETIF_F_HW_VLAN_CTAG_TX NETIF_F_HW_VLAN_TX
#endif

#endif
