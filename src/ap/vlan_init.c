/*
 * hostapd / VLAN initialization
 * Copyright 2003, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright (c) 2009, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include <net/if.h>
#include <sys/ioctl.h>
#ifdef CONFIG_FULL_DYNAMIC_VLAN
#include <linux/sockios.h>
#include <linux/if_vlan.h>
#include <linux/if_bridge.h>
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

#include "utils/common.h"
#include "hostapd.h"
#include "ap_config.h"
#include "ap_drv_ops.h"
#include "wpa_auth.h"
#include "vlan_init.h"
#include "vlan_util.h"


#ifdef CONFIG_FULL_DYNAMIC_VLAN

#include "drivers/priv_netlink.h"
#include "utils/eloop.h"


struct full_dynamic_vlan {
	int s; /* socket on which to listen for new/removed interfaces. */
};

#define DVLAN_CLEAN_BR         0x1
#define DVLAN_CLEAN_VLAN       0x2
#define DVLAN_CLEAN_VLAN_PORT  0x4

struct dynamic_iface {
	char ifname[IFNAMSIZ + 1];
	int usage;
	int clean;
	struct dynamic_iface *next;
};


/* Increment ref counter for ifname and add clean flag.
 * If not in list, add it only if some flags are given.
 */
static void dyn_iface_get(struct hostapd_data *hapd, const char *ifname,
			  int clean)
{
	struct dynamic_iface *next, **dynamic_ifaces;
	struct hapd_interfaces *interfaces;

	interfaces = hapd->iface->interfaces;
	dynamic_ifaces = &interfaces->vlan_priv;

	for (next = *dynamic_ifaces; next; next = next->next) {
		if (os_strcmp(ifname, next->ifname) == 0)
			break;
	}

	if (next) {
		next->usage++;
		next->clean |= clean;
		return;
	}

	if (!clean)
		return;

	next = os_zalloc(sizeof(*next));
	if (!next)
		return;
	os_strlcpy(next->ifname, ifname, sizeof(next->ifname));
	next->usage = 1;
	next->clean = clean;
	next->next = *dynamic_ifaces;
	*dynamic_ifaces = next;
}


/* Decrement reference counter for given ifname.
 * Return clean flag iff reference counter was decreased to zero, else zero
 */
static int dyn_iface_put(struct hostapd_data *hapd, const char *ifname)
{
	struct dynamic_iface *next, *prev = NULL, **dynamic_ifaces;
	struct hapd_interfaces *interfaces;
	int clean;

	interfaces = hapd->iface->interfaces;
	dynamic_ifaces = &interfaces->vlan_priv;

	for (next = *dynamic_ifaces; next; next = next->next) {
		if (os_strcmp(ifname, next->ifname) == 0)
			break;
		prev = next;
	}

	if (!next)
		return 0;

	next->usage--;
	if (next->usage)
		return 0;

	if (prev)
		prev->next = next->next;
	else
		*dynamic_ifaces = next->next;
	clean = next->clean;
	os_free(next);

	return clean;
}

#endif /* CONFIG_FULL_DYNAMIC_VLAN */


static int ifconfig_helper(const char *if_name, int up)
{
	int fd;
	struct ifreq ifr;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, if_name, IFNAMSIZ);

	if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: ioctl(SIOCGIFFLAGS) failed "
			   "for interface %s: %s",
			   __func__, if_name, strerror(errno));
		close(fd);
		return -1;
	}

	if (up)
		ifr.ifr_flags |= IFF_UP;
	else
		ifr.ifr_flags &= ~IFF_UP;

	if (ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: ioctl(SIOCSIFFLAGS) failed "
			   "for interface %s (up=%d): %s",
			   __func__, if_name, up, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


static int ifconfig_up(const char *if_name)
{
	wpa_printf(MSG_DEBUG, "VLAN: Set interface %s up", if_name);
	return ifconfig_helper(if_name, 1);
}


static int vlan_if_add(struct hostapd_data *hapd, struct hostapd_vlan *vlan,
		       int existsok)
{
	int ret, i;

	for (i = 0; i < NUM_WEP_KEYS; i++) {
		if (!hapd->conf->ssid.wep.key[i])
			continue;
		wpa_printf(MSG_ERROR,
			   "VLAN: Refusing to set up VLAN iface %s with WEP",
			   vlan->ifname);
		return -1;
	}

	if (!if_nametoindex(vlan->ifname))
		ret = hostapd_vlan_if_add(hapd, vlan->ifname);
	else if (!existsok)
		return -1;
	else
		ret = 0;

	if (ret)
		return ret;

	ifconfig_up(vlan->ifname); /* else wpa group will fail fatal */

	if (hapd->wpa_auth)
		ret = wpa_auth_ensure_group(hapd->wpa_auth, vlan->vlan_id);

	if (ret == 0)
		return ret;

	wpa_printf(MSG_ERROR, "WPA initialization for VLAN %d failed (%d)",
		   vlan->vlan_id, ret);
	if (wpa_auth_release_group(hapd->wpa_auth, vlan->vlan_id))
		wpa_printf(MSG_ERROR, "WPA deinit of %s failed", vlan->ifname);

	/* group state machine setup failed */
	if (hostapd_vlan_if_remove(hapd, vlan->ifname))
		wpa_printf(MSG_ERROR, "Removal of %s failed", vlan->ifname);

	return ret;
}


static int vlan_if_remove(struct hostapd_data *hapd, struct hostapd_vlan *vlan)
{
	int ret;

	ret = wpa_auth_release_group(hapd->wpa_auth, vlan->vlan_id);
	if (ret)
		wpa_printf(MSG_ERROR,
			   "WPA deinitialization for VLAN %d failed (%d)",
			   vlan->vlan_id, ret);

	return hostapd_vlan_if_remove(hapd, vlan->ifname);
}


#ifdef CONFIG_FULL_DYNAMIC_VLAN

static int ifconfig_down(const char *if_name)
{
	wpa_printf(MSG_DEBUG, "VLAN: Set interface %s down", if_name);
	return ifconfig_helper(if_name, 0);
}


/*
 * These are only available in recent linux headers (without the leading
 * underscore).
 */
#define _GET_VLAN_REALDEV_NAME_CMD	8
#define _GET_VLAN_VID_CMD		9

/* This value should be 256 ONLY. If it is something else, then hostapd
 * might crash!, as this value has been hard-coded in 2.4.x kernel
 * bridging code.
 */
#define MAX_BR_PORTS      		256

static int br_delif(const char *br_name, const char *if_name)
{
	int fd;
	struct ifreq ifr;
	unsigned long args[2];
	int if_index;

	wpa_printf(MSG_DEBUG, "VLAN: br_delif(%s, %s)", br_name, if_name);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	if_index = if_nametoindex(if_name);

	if (if_index == 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: Failure determining "
			   "interface index for '%s'",
			   __func__, if_name);
		close(fd);
		return -1;
	}

	args[0] = BRCTL_DEL_IF;
	args[1] = if_index;

	os_strlcpy(ifr.ifr_name, br_name, sizeof(ifr.ifr_name));
	ifr.ifr_data = (__caddr_t) args;

	if (ioctl(fd, SIOCDEVPRIVATE, &ifr) < 0 && errno != EINVAL) {
		/* No error if interface already removed. */
		wpa_printf(MSG_ERROR, "VLAN: %s: ioctl[SIOCDEVPRIVATE,"
			   "BRCTL_DEL_IF] failed for br_name=%s if_name=%s: "
			   "%s", __func__, br_name, if_name, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


/*
	Add interface 'if_name' to the bridge 'br_name'

	returns -1 on error
	returns 1 if the interface is already part of the bridge
	returns 0 otherwise
*/
static int br_addif(const char *br_name, const char *if_name)
{
	int fd;
	struct ifreq ifr;
	unsigned long args[2];
	int if_index;

	wpa_printf(MSG_DEBUG, "VLAN: br_addif(%s, %s)", br_name, if_name);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	if_index = if_nametoindex(if_name);

	if (if_index == 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: Failure determining "
			   "interface index for '%s'",
			   __func__, if_name);
		close(fd);
		return -1;
	}

	args[0] = BRCTL_ADD_IF;
	args[1] = if_index;

	os_strlcpy(ifr.ifr_name, br_name, sizeof(ifr.ifr_name));
	ifr.ifr_data = (__caddr_t) args;

	if (ioctl(fd, SIOCDEVPRIVATE, &ifr) < 0) {
		if (errno == EBUSY) {
			/* The interface is already added. */
			close(fd);
			return 1;
		}

		wpa_printf(MSG_ERROR, "VLAN: %s: ioctl[SIOCDEVPRIVATE,"
			   "BRCTL_ADD_IF] failed for br_name=%s if_name=%s: "
			   "%s", __func__, br_name, if_name, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


static int br_delbr(const char *br_name)
{
	int fd;
	unsigned long arg[2];

	wpa_printf(MSG_DEBUG, "VLAN: br_delbr(%s)", br_name);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	arg[0] = BRCTL_DEL_BRIDGE;
	arg[1] = (unsigned long) br_name;

	if (ioctl(fd, SIOCGIFBR, arg) < 0 && errno != ENXIO) {
		/* No error if bridge already removed. */
		wpa_printf(MSG_ERROR, "VLAN: %s: BRCTL_DEL_BRIDGE failed for "
			   "%s: %s", __func__, br_name, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


/*
	Add a bridge with the name 'br_name'.

	returns -1 on error
	returns 1 if the bridge already exists
	returns 0 otherwise
*/
static int br_addbr(const char *br_name)
{
	int fd;
	unsigned long arg[4];
	struct ifreq ifr;

	wpa_printf(MSG_DEBUG, "VLAN: br_addbr(%s)", br_name);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	arg[0] = BRCTL_ADD_BRIDGE;
	arg[1] = (unsigned long) br_name;

	if (ioctl(fd, SIOCGIFBR, arg) < 0) {
 		if (errno == EEXIST) {
			/* The bridge is already added. */
			close(fd);
			return 1;
		} else {
			wpa_printf(MSG_ERROR, "VLAN: %s: BRCTL_ADD_BRIDGE "
				   "failed for %s: %s",
				   __func__, br_name, strerror(errno));
			close(fd);
			return -1;
		}
	}

	/* Decrease forwarding delay to avoid EAPOL timeouts. */
	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, br_name, IFNAMSIZ);
	arg[0] = BRCTL_SET_BRIDGE_FORWARD_DELAY;
	arg[1] = 1;
	arg[2] = 0;
	arg[3] = 0;
	ifr.ifr_data = (char *) &arg;
	if (ioctl(fd, SIOCDEVPRIVATE, &ifr) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: "
			   "BRCTL_SET_BRIDGE_FORWARD_DELAY (1 sec) failed for "
			   "%s: %s", __func__, br_name, strerror(errno));
		/* Continue anyway */
	}

	close(fd);
	return 0;
}


static int br_getnumports(const char *br_name)
{
	int fd;
	int i;
	int port_cnt = 0;
	unsigned long arg[4];
	int ifindices[MAX_BR_PORTS];
	struct ifreq ifr;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	arg[0] = BRCTL_GET_PORT_LIST;
	arg[1] = (unsigned long) ifindices;
	arg[2] = MAX_BR_PORTS;
	arg[3] = 0;

	os_memset(ifindices, 0, sizeof(ifindices));
	os_strlcpy(ifr.ifr_name, br_name, sizeof(ifr.ifr_name));
	ifr.ifr_data = (__caddr_t) arg;

	if (ioctl(fd, SIOCDEVPRIVATE, &ifr) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: BRCTL_GET_PORT_LIST "
			   "failed for %s: %s",
			   __func__, br_name, strerror(errno));
		close(fd);
		return -1;
	}

	for (i = 1; i < MAX_BR_PORTS; i++) {
		if (ifindices[i] > 0) {
			port_cnt++;
		}
	}

	close(fd);
	return port_cnt;
}


#ifndef CONFIG_VLAN_NETLINK

int vlan_rem(const char *if_name)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_rem(%s)", if_name);
	if ((os_strlen(if_name) + 1) > sizeof(if_request.device1)) {
		wpa_printf(MSG_ERROR, "VLAN: Interface name too long: '%s'",
			   if_name);
		return -1;
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	os_strlcpy(if_request.device1, if_name, sizeof(if_request.device1));
	if_request.cmd = DEL_VLAN_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: DEL_VLAN_CMD failed for %s: "
			   "%s", __func__, if_name, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


/*
	Add a vlan interface with VLAN ID 'vid' and tagged interface
	'if_name'.

	returns -1 on error
	returns 1 if the interface already exists
	returns 0 otherwise
*/
int vlan_add(const char *if_name, int vid, const char *vlan_if_name)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_add(if_name=%s, vid=%d)",
		   if_name, vid);
	ifconfig_up(if_name);

	if ((os_strlen(if_name) + 1) > sizeof(if_request.device1)) {
		wpa_printf(MSG_ERROR, "VLAN: Interface name too long: '%s'",
			   if_name);
		return -1;
	}

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	/* Determine if a suitable vlan device already exists. */

	os_snprintf(if_request.device1, sizeof(if_request.device1), "vlan%d",
		    vid);

	if_request.cmd = _GET_VLAN_VID_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) == 0) {

		if (if_request.u.VID == vid) {
			if_request.cmd = _GET_VLAN_REALDEV_NAME_CMD;

			if (ioctl(fd, SIOCSIFVLAN, &if_request) == 0 &&
			    os_strncmp(if_request.u.device2, if_name,
				       sizeof(if_request.u.device2)) == 0) {
				close(fd);
				wpa_printf(MSG_DEBUG, "VLAN: vlan_add: "
					   "if_name %s exists already",
					   if_request.device1);
				return 1;
			}
		}
	}

	/* A suitable vlan device does not already exist, add one. */

	os_memset(&if_request, 0, sizeof(if_request));
	os_strlcpy(if_request.device1, if_name, sizeof(if_request.device1));
	if_request.u.VID = vid;
	if_request.cmd = ADD_VLAN_CMD;

	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: ADD_VLAN_CMD failed for %s: "
			   "%s",
			   __func__, if_request.device1, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}


static int vlan_set_name_type(unsigned int name_type)
{
	int fd;
	struct vlan_ioctl_args if_request;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_set_name_type(name_type=%u)",
		   name_type);
	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(AF_INET,SOCK_STREAM) "
			   "failed: %s", __func__, strerror(errno));
		return -1;
	}

	os_memset(&if_request, 0, sizeof(if_request));

	if_request.u.name_type = name_type;
	if_request.cmd = SET_VLAN_NAME_TYPE_CMD;
	if (ioctl(fd, SIOCSIFVLAN, &if_request) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: SET_VLAN_NAME_TYPE_CMD "
			   "name_type=%u failed: %s",
			   __func__, name_type, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

#endif /* CONFIG_VLAN_NETLINK */


static void vlan_newlink(char *ifname, struct hostapd_data *hapd)
{
	char vlan_ifname[IFNAMSIZ];
	char br_name[IFNAMSIZ];
	struct hostapd_vlan *vlan = hapd->conf->vlan;
	char *tagged_interface = hapd->conf->ssid.vlan_tagged_interface;
	int vlan_naming = hapd->conf->ssid.vlan_naming;
	int clean;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_newlink(%s)", ifname);

	while (vlan) {
		if (os_strcmp(ifname, vlan->ifname) == 0 && !vlan->configured) {
			vlan->configured = 1;

			if (hapd->conf->vlan_bridge[0]) {
				os_snprintf(br_name, sizeof(br_name), "%s%d",
					    hapd->conf->vlan_bridge,
					    vlan->vlan_id);
			} else if (tagged_interface) {
				os_snprintf(br_name, sizeof(br_name),
				            "br%s.%d", tagged_interface,
					    vlan->vlan_id);
			} else {
				os_snprintf(br_name, sizeof(br_name),
				            "brvlan%d", vlan->vlan_id);
			}

			dyn_iface_get(hapd, br_name,
				      br_addbr(br_name) ? 0 : DVLAN_CLEAN_BR);

			ifconfig_up(br_name);

			if (tagged_interface) {
				if (vlan_naming ==
				    DYNAMIC_VLAN_NAMING_WITH_DEVICE)
					os_snprintf(vlan_ifname,
						    sizeof(vlan_ifname),
						    "%s.%d", tagged_interface,
						    vlan->vlan_id);
				else
					os_snprintf(vlan_ifname,
						    sizeof(vlan_ifname),
						    "vlan%d", vlan->vlan_id);

				clean = 0;
				ifconfig_up(tagged_interface);
				if (!vlan_add(tagged_interface, vlan->vlan_id,
					      vlan_ifname))
					clean |= DVLAN_CLEAN_VLAN;

				if (!br_addif(br_name, vlan_ifname))
					clean |= DVLAN_CLEAN_VLAN_PORT;

				dyn_iface_get(hapd, vlan_ifname, clean);

				ifconfig_up(vlan_ifname);
			}

			if (!br_addif(br_name, ifname))
				vlan->clean |= DVLAN_CLEAN_WLAN_PORT;

			ifconfig_up(ifname);

			break;
		}
		vlan = vlan->next;
	}
}


static void vlan_dellink(char *ifname, struct hostapd_data *hapd)
{
	char vlan_ifname[IFNAMSIZ];
	char br_name[IFNAMSIZ];
	struct hostapd_vlan *first, *prev, *vlan = hapd->conf->vlan;
	char *tagged_interface = hapd->conf->ssid.vlan_tagged_interface;
	int vlan_naming = hapd->conf->ssid.vlan_naming;
	int clean;

	wpa_printf(MSG_DEBUG, "VLAN: vlan_dellink(%s)", ifname);

	first = prev = vlan;

	while (vlan) {
		if (os_strcmp(ifname, vlan->ifname) == 0 &&
		    vlan->configured) {
			if (hapd->conf->vlan_bridge[0]) {
				os_snprintf(br_name, sizeof(br_name), "%s%d",
					    hapd->conf->vlan_bridge,
					    vlan->vlan_id);
			} else if (tagged_interface) {
				os_snprintf(br_name, sizeof(br_name),
				            "br%s.%d", tagged_interface,
					    vlan->vlan_id);
			} else {
				os_snprintf(br_name, sizeof(br_name),
				            "brvlan%d", vlan->vlan_id);
			}

			if (vlan->clean & DVLAN_CLEAN_WLAN_PORT)
				br_delif(br_name, vlan->ifname);

			if (tagged_interface) {
				if (vlan_naming ==
				    DYNAMIC_VLAN_NAMING_WITH_DEVICE)
					os_snprintf(vlan_ifname,
						    sizeof(vlan_ifname),
						    "%s.%d", tagged_interface,
						    vlan->vlan_id);
				else
					os_snprintf(vlan_ifname,
						    sizeof(vlan_ifname),
						    "vlan%d", vlan->vlan_id);

				clean = dyn_iface_put(hapd, vlan_ifname);

				if (clean & DVLAN_CLEAN_VLAN_PORT)
					br_delif(br_name, vlan_ifname);

				if (clean & DVLAN_CLEAN_VLAN) {
					ifconfig_down(vlan_ifname);
					vlan_rem(vlan_ifname);
				}
			}

			clean = dyn_iface_put(hapd, br_name);
			if ((clean & DVLAN_CLEAN_BR) &&
			    br_getnumports(br_name) == 0) {
				ifconfig_down(br_name);
				br_delbr(br_name);
			}
		}

		if (os_strcmp(ifname, vlan->ifname) == 0) {
			if (vlan == first) {
				hapd->conf->vlan = vlan->next;
			} else {
				prev->next = vlan->next;
			}
			os_free(vlan);

			break;
		}
		prev = vlan;
		vlan = vlan->next;
	}
}


static void
vlan_read_ifnames(struct nlmsghdr *h, size_t len, int del,
		  struct hostapd_data *hapd)
{
	struct ifinfomsg *ifi;
	int attrlen, nlmsg_len, rta_len;
	struct rtattr *attr;
	char ifname[IFNAMSIZ + 1];

	if (len < sizeof(*ifi))
		return;

	ifi = NLMSG_DATA(h);

	nlmsg_len = NLMSG_ALIGN(sizeof(struct ifinfomsg));

	attrlen = h->nlmsg_len - nlmsg_len;
	if (attrlen < 0)
		return;

	attr = (struct rtattr *) (((char *) ifi) + nlmsg_len);

	os_memset(ifname, 0, sizeof(ifname));
	rta_len = RTA_ALIGN(sizeof(struct rtattr));
	while (RTA_OK(attr, attrlen)) {
		if (attr->rta_type == IFLA_IFNAME) {
			int n = attr->rta_len - rta_len;
			if (n < 0)
				break;

			if ((size_t) n >= sizeof(ifname))
				n = sizeof(ifname) - 1;
			os_memcpy(ifname, ((char *) attr) + rta_len, n);

		}

		attr = RTA_NEXT(attr, attrlen);
	}

	if (!ifname[0])
		return;
	if (del && if_nametoindex(ifname)) {
		 /* interface still exists, race condition ->
		  * iface has just been recreated */
		return;
	}

	wpa_printf(MSG_DEBUG,
		   "VLAN: RTM_%sLINK: ifi_index=%d ifname=%s ifi_family=%d ifi_flags=0x%x (%s%s%s%s)",
		   del ? "DEL" : "NEW",
		   ifi->ifi_index, ifname, ifi->ifi_family, ifi->ifi_flags,
		   (ifi->ifi_flags & IFF_UP) ? "[UP]" : "",
		   (ifi->ifi_flags & IFF_RUNNING) ? "[RUNNING]" : "",
		   (ifi->ifi_flags & IFF_LOWER_UP) ? "[LOWER_UP]" : "",
		   (ifi->ifi_flags & IFF_DORMANT) ? "[DORMANT]" : "");

	if (del)
		vlan_dellink(ifname, hapd);
	else
		vlan_newlink(ifname, hapd);
}


static void vlan_event_receive(int sock, void *eloop_ctx, void *sock_ctx)
{
	char buf[8192];
	int left;
	struct sockaddr_nl from;
	socklen_t fromlen;
	struct nlmsghdr *h;
	struct hostapd_data *hapd = eloop_ctx;

	fromlen = sizeof(from);
	left = recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT,
			(struct sockaddr *) &from, &fromlen);
	if (left < 0) {
		if (errno != EINTR && errno != EAGAIN)
			wpa_printf(MSG_ERROR, "VLAN: %s: recvfrom failed: %s",
				   __func__, strerror(errno));
		return;
	}

	h = (struct nlmsghdr *) buf;
	while (NLMSG_OK(h, left)) {
		int len, plen;

		len = h->nlmsg_len;
		plen = len - sizeof(*h);
		if (len > left || plen < 0) {
			wpa_printf(MSG_DEBUG, "VLAN: Malformed netlink "
				   "message: len=%d left=%d plen=%d",
				   len, left, plen);
			break;
		}

		switch (h->nlmsg_type) {
		case RTM_NEWLINK:
			vlan_read_ifnames(h, plen, 0, hapd);
			break;
		case RTM_DELLINK:
			vlan_read_ifnames(h, plen, 1, hapd);
			break;
		}

		h = NLMSG_NEXT(h, left);
	}

	if (left > 0) {
		wpa_printf(MSG_DEBUG, "VLAN: %s: %d extra bytes in the end of "
			   "netlink message", __func__, left);
	}
}


static struct full_dynamic_vlan *
full_dynamic_vlan_init(struct hostapd_data *hapd)
{
	struct sockaddr_nl local;
	struct full_dynamic_vlan *priv;

	priv = os_zalloc(sizeof(*priv));
	if (priv == NULL)
		return NULL;

#ifndef CONFIG_VLAN_NETLINK
	vlan_set_name_type(hapd->conf->ssid.vlan_naming ==
			   DYNAMIC_VLAN_NAMING_WITH_DEVICE ?
			   VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD :
			   VLAN_NAME_TYPE_PLUS_VID_NO_PAD);
#endif /* CONFIG_VLAN_NETLINK */

	priv->s = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (priv->s < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: socket(PF_NETLINK,SOCK_RAW,"
			   "NETLINK_ROUTE) failed: %s",
			   __func__, strerror(errno));
		os_free(priv);
		return NULL;
	}

	os_memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	local.nl_groups = RTMGRP_LINK;
	if (bind(priv->s, (struct sockaddr *) &local, sizeof(local)) < 0) {
		wpa_printf(MSG_ERROR, "VLAN: %s: bind(netlink) failed: %s",
			   __func__, strerror(errno));
		close(priv->s);
		os_free(priv);
		return NULL;
	}

	if (eloop_register_read_sock(priv->s, vlan_event_receive, hapd, NULL))
	{
		close(priv->s);
		os_free(priv);
		return NULL;
	}

	return priv;
}


static void full_dynamic_vlan_deinit(struct full_dynamic_vlan *priv)
{
	if (priv == NULL)
		return;
	eloop_unregister_read_sock(priv->s);
	close(priv->s);
	os_free(priv);
}
#endif /* CONFIG_FULL_DYNAMIC_VLAN */


static int vlan_dynamic_add(struct hostapd_data *hapd,
			    struct hostapd_vlan *vlan)
{
	while (vlan) {
		if (vlan->vlan_id != VLAN_ID_WILDCARD) {
			if (vlan_if_add(hapd, vlan, 1)) {
				wpa_printf(MSG_ERROR,
					   "VLAN: Could not add VLAN %s: %s",
					   vlan->ifname, strerror(errno));
				return -1;
			}
#ifdef CONFIG_FULL_DYNAMIC_VLAN
			vlan_newlink(vlan->ifname, hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
		}

		vlan = vlan->next;
	}

	return 0;
}


static void vlan_dynamic_remove(struct hostapd_data *hapd,
				struct hostapd_vlan *vlan)
{
	struct hostapd_vlan *next;

	while (vlan) {
		next = vlan->next;

		if (vlan->vlan_id != VLAN_ID_WILDCARD &&
		    vlan_if_remove(hapd, vlan)) {
			wpa_printf(MSG_ERROR, "VLAN: Could not remove VLAN "
				   "iface: %s: %s",
				   vlan->ifname, strerror(errno));
		}
#ifdef CONFIG_FULL_DYNAMIC_VLAN
		if (vlan->clean)
			vlan_dellink(vlan->ifname, hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

		vlan = next;
	}
}


int vlan_init(struct hostapd_data *hapd)
{
#ifdef CONFIG_FULL_DYNAMIC_VLAN
	hapd->full_dynamic_vlan = full_dynamic_vlan_init(hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */

	if (hapd->conf->ssid.dynamic_vlan != DYNAMIC_VLAN_DISABLED &&
	    !hapd->conf->vlan) {
		/* dynamic vlans enabled but no (or empty) vlan_file given */
		struct hostapd_vlan *vlan;
		vlan = os_zalloc(sizeof(*vlan));
		if (vlan == NULL) {
			wpa_printf(MSG_ERROR, "Out of memory while assigning "
				   "VLAN interfaces");
			return -1;
		}

		vlan->vlan_id = VLAN_ID_WILDCARD;
		os_snprintf(vlan->ifname, sizeof(vlan->ifname), "%s.#",
			    hapd->conf->iface);
		vlan->next = hapd->conf->vlan;
		hapd->conf->vlan = vlan;
	}

	if (vlan_dynamic_add(hapd, hapd->conf->vlan))
		return -1;

        return 0;
}


void vlan_deinit(struct hostapd_data *hapd)
{
	vlan_dynamic_remove(hapd, hapd->conf->vlan);

#ifdef CONFIG_FULL_DYNAMIC_VLAN
	full_dynamic_vlan_deinit(hapd->full_dynamic_vlan);
	hapd->full_dynamic_vlan = NULL;
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
}


struct hostapd_vlan * vlan_add_dynamic(struct hostapd_data *hapd,
				       struct hostapd_vlan *vlan,
				       int vlan_id)
{
	struct hostapd_vlan *n = NULL;
	char *ifname, *pos;

	if (vlan == NULL || vlan_id <= 0 || vlan_id > MAX_VLAN_ID ||
	    vlan->vlan_id != VLAN_ID_WILDCARD)
		return NULL;

	wpa_printf(MSG_DEBUG, "VLAN: %s(vlan_id=%d ifname=%s)",
		   __func__, vlan_id, vlan->ifname);
	ifname = os_strdup(vlan->ifname);
	if (ifname == NULL)
		return NULL;
	pos = os_strchr(ifname, '#');
	if (pos == NULL)
		goto free_ifname;
	*pos++ = '\0';

	n = os_zalloc(sizeof(*n));
	if (n == NULL)
		goto free_ifname;

	n->vlan_id = vlan_id;
	n->dynamic_vlan = 1;

	os_snprintf(n->ifname, sizeof(n->ifname), "%s%d%s", ifname, vlan_id,
		    pos);

	n->next = hapd->conf->vlan;
	hapd->conf->vlan = n;

	/* hapd->conf->vlan needs this new VLAN here for WPA setup */
	if (vlan_if_add(hapd, n, 0)) {
		hapd->conf->vlan = n->next;
		os_free(n);
		n = NULL;
		goto free_ifname;
	}

free_ifname:
	os_free(ifname);
	return n;
}


int vlan_remove_dynamic(struct hostapd_data *hapd, int vlan_id)
{
	struct hostapd_vlan *vlan;

	if (vlan_id <= 0 || vlan_id > MAX_VLAN_ID)
		return 1;

	wpa_printf(MSG_DEBUG, "VLAN: %s(ifname=%s vlan_id=%d)",
		   __func__, hapd->conf->iface, vlan_id);

	vlan = hapd->conf->vlan;
	while (vlan) {
		if (vlan->vlan_id == vlan_id && vlan->dynamic_vlan > 0) {
			vlan->dynamic_vlan--;
			break;
		}
		vlan = vlan->next;
	}

	if (vlan == NULL)
		return 1;

	if (vlan->dynamic_vlan == 0) {
		vlan_if_remove(hapd, vlan);
#ifdef CONFIG_FULL_DYNAMIC_VLAN
		vlan_dellink(vlan->ifname, hapd);
#endif /* CONFIG_FULL_DYNAMIC_VLAN */
	}

	return 0;
}
