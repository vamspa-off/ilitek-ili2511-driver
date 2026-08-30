# ilitek-ili2511-driver
ilitek ili2511 i2c driver for linux with kernel 2.6.35.4

Add content from Kconfig and Makefile to your Kconfig and Makefile.

Realized functions:
2-finger support
registration in linux input subsystem
swap X and Y axes
invert X or/and Y axes

You can use compiled driver

Line below imports driver and swaps X and Y, inverts X and Y
```bash
insmod ilitek_i2c.ko swap_xy=1 invert_x=1 invert_y=1
```
