#ifndef __CONSOLE_PC9800_H
#define __CONSOLE_PC9800_H

#define PC9800_VRAM_ATTR_OFFSET 0x2000
static inline unsigned long __pc9800_attr_offset (unsigned long Addr, unsigned long vss)
{
	if(Addr >= (unsigned long)__va(0xa0000)
	   && Addr < (unsigned long)__va(0xa2000)) {
		Addr += PC9800_VRAM_ATTR_OFFSET;
	}else{
		Addr += vss;
	}
	return Addr;
}
#define pc9800_attr_offset(x) \
	((typeof(x))__pc9800_attr_offset((unsigned long)(x),(vc_cons[currcons].d->vc_screenbuf_size)))
#define BLANK_ATTR	0x00E1

#define JIS_CODE       0x01
#define EUC_CODE       0x00
#define SJIS_CODE      0x02
#define JIS_CODE_ASCII  0x00
#define JIS_CODE_78     0x01
#define JIS_CODE_83     0x02
#define JIS_CODE_90     0x03

#endif /* __CONSOLE_PC9800_H */
