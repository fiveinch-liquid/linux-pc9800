#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/apm_bios.h>
#include <linux/slab.h>
#include <asm/io.h>

struct dmi_header
{
	u8	type;
	u8	length;
	u16	handle;
};

#define dmi_printk(x)
//#define dmi_printk(x) printk(x)

static char * __init dmi_string(struct dmi_header *dm, u8 s)
{
	u8 *bp=(u8 *)dm;
	bp+=dm->length;
	s--;
	while(s>0)
	{
		bp+=strlen(bp);
		bp++;
		s--;
	}
	return bp;
}

static int __init dmi_table(u32 base, int len, int num, void (*decode)(struct dmi_header *))
{
	u8 *buf;
	struct dmi_header *dm;
	u8 *data;
	int i=1;
	int last = 0;	
		
	buf = ioremap(base, len);
	if(buf==NULL)
		return -1;

	data = buf;
	while(i<num && (data - buf) < len)
	{
		dm=(struct dmi_header *)data;
		if(dm->type < last)
			break;
		last = dm->type;
		decode(dm);		
		data+=dm->length;
		while(*data || data[1])
			data++;
		data+=2;
		i++;
	}
	iounmap(buf);
	return 0;
}


int __init dmi_iterate(void (*decode)(struct dmi_header *))
{
	unsigned char buf[20];
	long fp=0xE0000L;
	fp -= 16;
	
	while( fp < 0xFFFFF)
	{
		fp+=16;
		isa_memcpy_fromio(buf, fp, 20);
		if(memcmp(buf, "_DMI_", 5)==0)
		{
			u16 num=buf[13]<<8|buf[12];
			u16 len=buf[7]<<8|buf[6];
			u32 base=buf[11]<<24|buf[10]<<16|buf[9]<<8|buf[8];

			dmi_printk((KERN_INFO "DMI %d.%d present.\n",
				buf[14]>>4, buf[14]&0x0F));
			dmi_printk((KERN_INFO "%d structures occupying %d bytes.\n",
				buf[13]<<8|buf[12],
				buf[7]<<8|buf[6]));
			dmi_printk((KERN_INFO "DMI table at 0x%08X.\n",
				buf[11]<<24|buf[10]<<16|buf[9]<<8|buf[8]));
			if(dmi_table(base,len, num, decode)==0)
				return 0;
		}
	}
	return -1;
}


enum
{
	DMI_BIOS_VENDOR,
	DMI_BIOS_VERSION,
	DMI_BIOS_DATE,
	DMI_SYS_VENDOR,
	DMI_PRODUCT_NAME,
	DMI_PRODUCT_VERSION,
	DMI_BOARD_VENDOR,
	DMI_BOARD_NAME,
	DMI_BOARD_VERSION,
	DMI_STRING_MAX
};

static char *dmi_ident[DMI_STRING_MAX];

/*
 *	Save a DMI string
 */
 
static void __init dmi_save_ident(struct dmi_header *dm, int slot, int string)
{
	char *d = (char*)dm;
	char *p = dmi_string(dm, d[string]);
	if(p==NULL || *p == 0)
		return;
	if (dmi_ident[slot])
		return;
	dmi_ident[slot] = kmalloc(strlen(p)+1, GFP_KERNEL);
	if(dmi_ident[slot])
		strcpy(dmi_ident[slot], p);
	else
		printk(KERN_ERR "dmi_save_ident: out of memory.\n");
}

/*
 *	DMI callbacks for problem boards
 */

struct dmi_strmatch
{
	u8 slot;
	char *substr;
};

#define NONE	255

struct dmi_blacklist
{
	int (*callback)(struct dmi_blacklist *);
	char *ident;
	struct dmi_strmatch matches[4];
};

#define NO_MATCH	{ NONE, NULL}
#define MATCH(a,b)	{ a, b }

/*
 *	We have problems with IDE DMA on some platforms. In paticular the
 *	KT7 series. On these it seems the newer BIOS has fixed them. The
 *	rule needs to be improved to match specific BIOS revisions with
 *	corruption problems
 */ 
 
static __init int disable_ide_dma(struct dmi_blacklist *d)
{
#ifdef CONFIG_BLK_DEV_IDE
	extern int noautodma;
	if(noautodma == 0)
	{
		noautodma = 1;
		printk(KERN_INFO "%s series board detected. Disabling IDE DMA.\n", d->ident);
	}
#endif	
	return 0;
}

/* 
 * Some machines require the "reboot=b"  commandline option, this quirk makes that automatic.
 */
static __init int set_bios_reboot(struct dmi_blacklist *d)
{
	extern int reboot_thru_bios;
	if (reboot_thru_bios == 0)
	{
		reboot_thru_bios = 1;
		printk(KERN_INFO "%s series board detected. Selecting BIOS-method for reboots.\n", d->ident);
	}
	return 0;
}

/*
 * Some bioses have a broken protected mode poweroff and need to use realmode
 */

static __init int set_realmode_power_off(struct dmi_blacklist *d)
{
       if (apm_info.realmode_power_off == 0)
       {
               apm_info.realmode_power_off = 1;
               printk(KERN_INFO "%s bios detected. Using realmode poweroff only.\n", d->ident);
       }
       return 0;
}


/* 
 * Some laptops require interrupts to be enabled during APM calls 
 */

static __init int set_apm_ints(struct dmi_blacklist *d)
{
	if (apm_info.allow_ints == 0)
	{
		apm_info.allow_ints = 1;
		printk(KERN_INFO "%s machine detected. Enabling interrupts during APM calls.\n", d->ident);
	}
	return 0;
}

/* 
 * Some APM bioses corrupt memory or just plain do not work
 */

static __init int apm_is_horked(struct dmi_blacklist *d)
{
	if (apm_info.disabled == 0)
	{
		apm_info.disabled = 1;
		printk(KERN_INFO "%s machine detected. Disabling APM.\n", d->ident);
	}
	return 0;
}


/*
 *  Check for clue free BIOS implementations who use
 *  the following QA technique
 *
 *      [ Write BIOS Code ]<------
 *               |                ^
 *      < Does it Compile >----N--
 *               |Y               ^
 *	< Does it Boot Win98 >-N--
 *               |Y
 *           [Ship It]
 *
 *	Phoenix A04  08/24/2000 is known bad (Dell Inspiron 5000e)
 *	Phoenix A07  09/29/2000 is known good (Dell Inspiron 5000)
 */

static __init int broken_apm_power(struct dmi_blacklist *d)
{
	apm_info.get_power_status_broken = 1;
	printk(KERN_WARNING "BIOS strings suggest APM bugs, disabling power status reporting.\n");
	return 0;
}		

/*
 *	Process the DMI blacklists
 */
 

/*
 *	This will be expanded over time to force things like the APM 
 *	interrupt mask settings according to the laptop
 */
 
static __initdata struct dmi_blacklist dmi_blacklist[]={
#if 0
	{ disable_ide_dma, "KT7", {	/* Overbroad right now - kill DMA on problem KT7 boards */
			MATCH(DMI_PRODUCT_NAME, "KT7-RAID"),
			NO_MATCH, NO_MATCH, NO_MATCH
			} },
#endif			
	{ broken_apm_power, "Dell Inspiron 5000e", {	/* Handle problems with APM on Inspiron 5000e */
			MATCH(DMI_BIOS_VENDOR, "Phoenix Technologies LTD"),
			MATCH(DMI_BIOS_VERSION, "A04"),
			MATCH(DMI_BIOS_DATE, "08/24/2000"), NO_MATCH
			} },
	{ set_realmode_power_off, "Award Software v4.60 PGMA", {	/* broken PM poweroff bios */
			MATCH(DMI_BIOS_VENDOR, "Award Software International, Inc."),
			MATCH(DMI_BIOS_VERSION, "4.60 PGMA"),
			MATCH(DMI_BIOS_DATE, "134526184"), NO_MATCH
			} },
	{ set_bios_reboot, "PowerEdge 1300/500", {	/* Handle problems with rebooting on Dell 1300's */
			MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
			MATCH(DMI_PRODUCT_NAME, "PowerEdge 1300/500"),
			NO_MATCH, NO_MATCH
			} },
	{ set_bios_reboot, "PowerEdge 1300/550", {	/* Handle problems with rebooting on Dell 1300's */
			MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
			MATCH(DMI_PRODUCT_NAME, "PowerEdge 1300/550"),
			NO_MATCH, NO_MATCH
			} },
	{ set_apm_ints, "IBM", {	/* Allow interrupts during suspend on IBM laptops */
			MATCH(DMI_SYS_VENDOR, "IBM"),
			NO_MATCH, NO_MATCH, NO_MATCH
			} },
	{ set_apm_ints, "ASUSTeK", {   /* Allow interrupts during APM or the clock goes slow */
			MATCH(DMI_SYS_VENDOR, "ASUSTeK Computer Inc."),
			MATCH(DMI_PRODUCT_NAME, "L8400K series Notebook PC"),
			NO_MATCH, NO_MATCH
			} },					
	{ apm_is_horked, "Trigem Delhi3", { /* APM crashes */
			MATCH(DMI_SYS_VENDOR, "TriGem Computer, Inc"),
			MATCH(DMI_PRODUCT_NAME, "Delhi3"),
			NO_MATCH, NO_MATCH,
			} },
	{ NULL, }
};
	
	
/*
 *	Walk the blacklist table running matching functions until someone 
 *	returns 1 or we hit the end.
 */
 
static __init void dmi_check_blacklist(void)
{
	struct dmi_blacklist *d;
	int i;
		
	d=&dmi_blacklist[0];
	while(d->callback)
	{
		for(i=0;i<4;i++)
		{
			int s = d->matches[i].slot;
			if(s==NONE)
				continue;
			if(dmi_ident[s] && strstr(dmi_ident[s], d->matches[i].substr))
				continue;
			/* No match */
			goto fail;
		}
		if(d->callback(d))
			return;
fail:			
		d++;
	}
}

	

/*
 *	Process a DMI table entry. Right now all we care about are the BIOS
 *	and machine entries. For 2.5 we should pull the smbus controller info
 *	out of here.
 */

static void __init dmi_decode(struct dmi_header *dm)
{
	u8 *data = (u8 *)dm;
	char *p;
	
	switch(dm->type)
	{
		case  0:
			p=dmi_string(dm,data[4]);
			if(*p)
			{
				dmi_printk(("BIOS Vendor: %s\n", p));
				dmi_save_ident(dm, DMI_BIOS_VENDOR, 4);
				dmi_printk(("BIOS Version: %s\n", 
					dmi_string(dm, data[5])));
				dmi_save_ident(dm, DMI_BIOS_VERSION, 5);
				dmi_printk(("BIOS Release: %s\n",
					dmi_string(dm, data[8])));
				dmi_save_ident(dm, DMI_BIOS_DATE, 8);
			}
			break;
			
		case 1:
			p=dmi_string(dm,data[4]);
			if(*p)
			{
				dmi_printk(("System Vendor: %s.\n",p));
				dmi_save_ident(dm, DMI_SYS_VENDOR, 4);
				dmi_printk(("Product Name: %s.\n",
					dmi_string(dm, data[5])));
				dmi_save_ident(dm, DMI_PRODUCT_NAME, 5);
				dmi_printk(("Version %s.\n",
					dmi_string(dm, data[6])));
				dmi_save_ident(dm, DMI_PRODUCT_VERSION, 6);
				dmi_printk(("Serial Number %s.\n",
					dmi_string(dm, data[7])));
			}
			break;
		case 2:
			p=dmi_string(dm,data[4]);
			if(*p)
			{
				dmi_printk(("Board Vendor: %s.\n",p));
				dmi_save_ident(dm, DMI_BOARD_VENDOR, 4);
				dmi_printk(("Board Name: %s.\n",
					dmi_string(dm, data[5])));
				dmi_save_ident(dm, DMI_BOARD_NAME, 5);
				dmi_printk(("Board Version: %s.\n",
					dmi_string(dm, data[6])));
				dmi_save_ident(dm, DMI_BOARD_VERSION, 6);
			}
			break;
		case 3:
			p=dmi_string(dm,data[8]);
			if(*p && *p!=' ')
				dmi_printk(("Asset Tag: %s.\n", p));
			break;
	}
}

static int __init dmi_scan_machine(void)
{
	int err = dmi_iterate(dmi_decode);
	if(err == 0)
		dmi_check_blacklist();
	return err;
}

module_init(dmi_scan_machine);
