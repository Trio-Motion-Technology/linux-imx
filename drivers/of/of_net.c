// SPDX-License-Identifier: GPL-2.0-only
/*
 * OF helpers for network devices.
 *
 * Initially copied out of arch/powerpc/kernel/prom_parse.c
 */
#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/export.h>
#include <linux/device.h>

/**
 * of_get_phy_mode - Get phy mode for given device_node
 * @np:	Pointer to the given device_node
 *
 * The function gets phy interface string from property 'phy-mode' or
 * 'phy-connection-type', and return its index in phy_modes table, or errno in
 * error case.
 */
int of_get_phy_mode(struct device_node *np)
{
	const char *pm;
	int err, i;

	err = of_property_read_string(np, "phy-mode", &pm);
	if (err < 0)
		err = of_property_read_string(np, "phy-connection-type", &pm);
	if (err < 0)
		return err;

	for (i = 0; i < PHY_INTERFACE_MODE_MAX; i++)
		if (!strcasecmp(pm, phy_modes(i)))
			return i;

	return -ENODEV;
}
EXPORT_SYMBOL_GPL(of_get_phy_mode);

static const void *of_get_mac_addr(struct device_node *np, const char *name)
{
	struct property *pp = of_find_property(np, name, NULL);

	if (pp && pp->length == ETH_ALEN && is_valid_ether_addr(pp->value))
		return pp->value;
	return NULL;
}

static const void *of_get_mac_addr_nvmem(struct device_node *np)
{
	int ret;
	const void *mac;
	u8 nvmem_mac[ETH_ALEN];
	struct platform_device *pdev = of_find_device_by_node(np);

	if (!pdev)
		return ERR_PTR(-ENODEV);

	ret = nvmem_get_mac_address(&pdev->dev, &nvmem_mac);
	if (ret) {
		put_device(&pdev->dev);
		return ERR_PTR(ret);
	}

	mac = devm_kmemdup(&pdev->dev, nvmem_mac, ETH_ALEN, GFP_KERNEL);
	put_device(&pdev->dev);
	if (!mac)
		return ERR_PTR(-ENOMEM);

	return mac;
}


#ifdef CONFIG_PLAT_TRIOMOTION
#ifdef CONFIG_TRIO_FLEX7_MIDI
#define NUM_TRIO_MACS 2
#define BASE_MAC_ADDRESS 0x001EFBF80001
#endif
#endif

#ifdef NUM_TRIO_MACS

u64 trio_generate_mac(int idx)
{
  	struct device_node *root;
   const char *pserial=NULL;
   long long serial_num = 0;
   u64 trio_mac = BASE_MAC_ADDRESS + idx; 
   
	root = of_find_node_by_path("/");
	if (root) {
		of_property_read_string(root, "serial-number",
					      &pserial);
      if (pserial!=NULL)
      {
         int ret= kstrtoll(pserial,0,&serial_num);
         if ((ret<0) || (serial_num<1) || (serial_num >= 229375))
         {
            serial_num =0;
            printk(KERN_ERR "Trio serial number out of range\n");
         }
         trio_mac= BASE_MAC_ADDRESS + (serial_num * NUM_TRIO_MACS) + idx ;
         printk(KERN_INFO "Trio generated MAC %d %012llX\n",idx,trio_mac);
      }
   }
   return trio_mac;
}

u64 return_trio_mac(int idx)
{
   return trio_generate_mac(idx);
}

u64 return_trio_mac_reversed(int idx) /* reverses byte order in bottom 6 bytes */
{   
   u64 forward= return_trio_mac(idx);
   u64 reversed;
   reversed  = (forward >> 40) & 0x00000000000000FF;
   reversed |= (forward >> 24) & 0x000000000000FF00;
   reversed |= (forward >> 8)  & 0x0000000000FF0000;
   reversed |= (forward << 8)  & 0x00000000FF000000;
   reversed |= (forward << 24) & 0x000000FF00000000;
   reversed |= (forward << 40) & 0x0000FF0000000000;
   return reversed;
}

EXPORT_SYMBOL(return_trio_mac);
EXPORT_SYMBOL(return_trio_mac_reversed);



static const void *of_get_trio_mac_addr(struct device_node *np)
{
   u32 idx;
   int ret;
   const void *mac;
	u8 trio_mac[ETH_ALEN];
   struct platform_device *pdev = of_find_device_by_node(np);
   
   if (!pdev)
   	return ERR_PTR(-ENODEV);
   
   ret = of_property_read_u32(np, "trio-mac-idx", &idx);
   if (ret >= 0)
   {
      u64 val= return_trio_mac(idx);
      trio_mac[0]= (val >> 40) & 0xff;
      trio_mac[1]= (val >> 32) & 0xff;
      trio_mac[2]= (val >> 24) & 0xff;
      trio_mac[3]= (val >> 16) & 0xff;
      trio_mac[4]= (val >> 8)  & 0xff;
      trio_mac[5]= (val)       & 0xff;
      mac = devm_kmemdup(&pdev->dev, trio_mac, ETH_ALEN, GFP_KERNEL);
      put_device(&pdev->dev);
      if (!mac)
         return ERR_PTR(-ENOMEM);

      return mac;
   }
	return NULL;
}
#endif // NUM_TRIO_MACS

/**
 * Search the device tree for the best MAC address to use.  'mac-address' is
 * checked first, because that is supposed to contain to "most recent" MAC
 * address. If that isn't set, then 'local-mac-address' is checked next,
 * because that is the default address. If that isn't set, then the obsolete
 * 'address' is checked, just in case we're using an old device tree. If any
 * of the above isn't set, then try to get MAC address from nvmem cell named
 * 'mac-address'.
 *
 * Note that the 'address' property is supposed to contain a virtual address of
 * the register set, but some DTS files have redefined that property to be the
 * MAC address.
 *
 * All-zero MAC addresses are rejected, because those could be properties that
 * exist in the device tree, but were not set by U-Boot.  For example, the
 * DTS could define 'mac-address' and 'local-mac-address', with zero MAC
 * addresses.  Some older U-Boots only initialized 'local-mac-address'.  In
 * this case, the real MAC is in 'local-mac-address', and 'mac-address' exists
 * but is all zeros.
 *
 * Return: Will be a valid pointer on success and ERR_PTR in case of error.
*/
const void *of_get_mac_address(struct device_node *np)
{
	const void *addr;

	addr = of_get_mac_addr(np, "mac-address");
	if (addr)
		return addr;

#ifdef NUM_TRIO_MACS
	addr = of_get_trio_mac_addr(np);
	if (addr)
		return addr;
#endif

	addr = of_get_mac_addr(np, "local-mac-address");
	if (addr)
		return addr;

	addr = of_get_mac_addr(np, "address");
	if (addr)
		return addr;

	addr = of_get_mac_addr(np, "nvmem-mac-address");
	if (addr)
		return addr;

	return of_get_mac_addr_nvmem(np);
}
EXPORT_SYMBOL(of_get_mac_address);
