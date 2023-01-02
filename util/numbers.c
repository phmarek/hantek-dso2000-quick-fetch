// vim: sw=4 ts=4 et : 


#define stringify(x) #x
#define DSO_KEY(nr, name) [nr] = stringify(name)

char *key_names[] =
{


    DSO_KEY(1, F0),
    DSO_KEY(2, F1),
    DSO_KEY(3, F2),
    DSO_KEY(4, F3),
    DSO_KEY(5, F4),
    DSO_KEY(6, F5),
    DSO_KEY(7, F6),
    DSO_KEY(8, TRIG),
    DSO_KEY(9, SAVE_RECALL),
    DSO_KEY(10, ACQ),
    DSO_KEY(11, DISPLAY),
    DSO_KEY(12, HORIZ),
    DSO_KEY(13, DECODE),
    DSO_KEY(14, UTILITY),
    DSO_KEY(15, CH1),
    DSO_KEY(16, CH2),
    DSO_KEY(17, MATH),
    DSO_KEY(18, MEASURE),
    DSO_KEY(19, AUTOSET),
    DSO_KEY(20, SINGLE_TRIG),
    DSO_KEY(21, RUN_STOP),
    DSO_KEY(22, SET_TRIG_50P),
    DSO_KEY(23, FORCE_TRIG),
    DSO_KEY(24, CURSOR),
    DSO_KEY(25, PROBE_CHECK),
    DSO_KEY(26, HELP),
    DSO_KEY(27, SAVE_TO_USB),
    DSO_KEY(28, DEFAULT_SET),
//    DSO_KEY(29, NM),
    DSO_KEY(29, PUSHKNOB_CH1_VERTIC_VOLT),
    DSO_KEY(30, PUSHKNOB_CH2_VERTIC_VOLT),
    DSO_KEY(31, PUSHKNOB_HORIZ_TIME),
    DSO_KEY(32, PUSHKNOB_TRIG_VOLT),
    DSO_KEY(33, PUSHKNOB_MULTI),
    DSO_KEY(36, PUSHKNOB_TIME_BASE),
    DSO_KEY(37, CH1_VERTIC_VOLT_TURN_RIGHT),
    DSO_KEY(38, CH1_VERTIC_VOLT_TURN_LEFT),
    DSO_KEY(39, CH2_VERTIC_VOLT_TURN_LEFT),
    DSO_KEY(40, CH2_VERTIC_VOLT_TURN_RIGHT),
    DSO_KEY(41, HORIZ_TIME_TURN_RIGHT),
    DSO_KEY(42, HORIZ_TIME_TURN_LEFT),
    DSO_KEY(43, TRIG_VOLT_TURN_RIGHT),
    DSO_KEY(44, TRIG_VOLT_TURN_LEFT),
    DSO_KEY(46, MULTI_TURN_LEFT),
    DSO_KEY(45, MULTI_TURN_RIGHT),
    DSO_KEY(47, CH1_VOLT_BASE_TURN_LEFT),
    DSO_KEY(48, CH1_VOLT_BASE_TURN_RIGHT),
    DSO_KEY(49, CH2_VOLT_BASE_TURN_RIGHT),
    DSO_KEY(50, CH2_VOLT_BASE_TURN_LEFT),
    DSO_KEY(51, TIME_BASE_TURN_LEFT),
    DSO_KEY(52, TIME_BASE_TURN_RIGHT),
};

#define DSO_LED(x) stringify(x)
//enum LedID
char *led_names[] =
{
    NULL,
    DSO_LED(CH1),
    DSO_LED(CH2),
    DSO_LED(MATH),
    DSO_LED(AUTOSET),
    DSO_LED(STOP),
    DSO_LED(RUN),
    DSO_LED(MULT),

    // LED_AFG3050_NM
};


// Adapted from drive/fpga_iic_drive/kb_test.c

int wait_report_key(int loop)
{ 
    int keys_fd;  
    struct input_event t;  
    struct input_event ievent;
    int same = 0;
    int last = 0;

    keys_fd = open("/dev/input/event0", O_RDWR);  
    if (keys_fd <= 0)  
    {  
        perror("open /dev/input/event0 device error!\n");  
        return 1;
    }  

    do {
        if (read (keys_fd, &t, sizeof(struct input_event)) == sizeof(struct input_event))  
        {  
            if(t.type == EV_KEY)
            {
                switch (loop)
                {
                    case 2:
                        if (t.value == 0) {
                            char *name;

                            name = (t.code < sizeof(key_names)/sizeof(key_names[0])) 
                                ? key_names[t.code] : NULL;
                            if (name)
                                printf("%s\n", name);
                            else
                                printf("#%d\n", t.code);
                        }
                        break;
                    case 1:
                        if (t.value == 0 || t.value == 1)  
                        {
                            printf ("%s %d %s\n", 
                                    (t.value) ? "Pressed" : "Released",
                                    t.code,  
                                    (t.code < sizeof(key_names)/sizeof(key_names[0])) 
                                    ? key_names[t.code] : "UNKNOWN"
                                   );  
                            if (t.value==1) {
                                if (t.code == same) {
                                    last++;
                                    if (last > 3) break;
                                }
                                same = t.code;
                            }
                        }
                } 
            }
            else if(t.type == EV_REL)
            {
                printf("0x%x, 0x%x, 0x%x, 0x%x,0x%x\n", t.type, t.value, t.code, EV_KEY,EV_REL);
            }
        } 

        fflush(stdout);
    } while (loop);

    close (keys_fd);  
    return 0;  
}

