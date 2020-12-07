# friendly_1wire_touchscreen
友善之臂单线触摸屏驱动

one-wire: 使用延时方式设置单线触摸屏的背光值，仅设置背光
mini6410_1wire_interrupt: 使用定时器2产生中断，在中断中传输数据（友善之臂的方式）
mini6410_1wire_hrtimer: 使用高精度定时器来进行传输数据（相对第二种，无需考虑硬件定时器配置、中断，操作也更加安全)

