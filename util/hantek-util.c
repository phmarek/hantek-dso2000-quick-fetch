// vim: sw=4 ts=4 et : 
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <strings.h>
#include <fcntl.h>
#include <err.h>
#include <getopt.h>
#include <stdio.h>
#include <linux/input.h>


#define ANOLIS_FN(x,r) extern x
#include "libanolis.c"


#include "numbers.c"

enum e_opts {
    enable_keypad = 200,
    disable_keypad = 201,
    lightup_key = 'l',
    darken_key = 'L',
    get_key = 'g',
    get_keys = 'G',
    keys = 'k',
};

const struct option opts[] = {
    { .name = "enable-keypad",  .has_arg = 0, .flag = NULL, .val = enable_keypad  },
    { .name = "disable-keypad", .has_arg = 0, .flag = NULL, .val = disable_keypad },
    { .name = "light-up-key",   .has_arg = 1, .flag = NULL, .val = lightup_key  },
    { .name = "darken-key",     .has_arg = 1, .flag = NULL, .val = darken_key },
    { .name = "get-key",        .has_arg = 0, .flag = NULL, .val = get_key },
    { .name = "get-keys",       .has_arg = 0, .flag = NULL, .val = get_keys },
    { .name = "keys",           .has_arg = 0, .flag = NULL, .val = keys },
    { 0 }
};

#define F fprintf(stderr, "at %d\n", __LINE__);

int main(int argc, char *args[])
{
    int opt, light, key;


    if (argc == 1) {
		printf("Options:\n");
        for(opt = 0; ; opt++) {
            if (!opts[opt].name)
                break;

            printf("  --%-20s  -%c\n", opts[opt].name, opts[opt].val);
        }
        exit(0);
    }

    opt = anolis_main();
//    printf("anolis_main says %d\n", opt);

    while (1) {
        opt = getopt_long(argc, args, "kKl:L:gG", opts, NULL);
        if (opt == -1) {
            break;
        }

        light = 0;

        switch (opt) {
            case enable_keypad: 
                anolis_unlock_keypad(); 
                break;
            case disable_keypad: 
                anolis_lock_keypad(); 
                break;

            case lightup_key: 
                light = 1;
            case darken_key: 
                if (!optarg) {
                    errx(1, "Option %c needs an argument\n", opt);
                }
                key = atoi(optarg);
				if (key == 0) {
					for(key = sizeof(led_names)/sizeof(led_names[0]) -1; key>=0; key--) {
						if (led_names[key] && strcasecmp(optarg, led_names[key]) == 0)
							break;
					}
					if (key == -1)
						errx(1, "Key %s unknown\n", optarg);
				}

                anolis_light_key(key, light);
                break;
			case get_key:
				wait_report_key(0);
				break;
			case get_keys:
				wait_report_key(1);
				break;
			case keys:
				wait_report_key(2);
				break;

/*
            case menu:
                anolis_get_menu_builder()
                    anolis_menu_builder_build_with_file(builder, "/dso/app/res/menu/hori_menu.xml", 1)

                    (gdb) call anolis_make_question("title", "foobar", 0x9c0500)
$2 = 11471480
(gdb) call anolis_msg_dialog_set_on_dialog_done_listener($2, 0x361a4, 0)
*/

        }
    }

    return 0;
}
