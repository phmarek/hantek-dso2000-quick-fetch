
#ifndef ANOLIS_FN
#define ANOLIS_FN(x,y) extern x
#endif


ANOLIS_FN(void anolis_lock_keypad(),);
ANOLIS_FN(void anolis_unlock_keypad(),);
ANOLIS_FN(void anolis_light_key(int key, int light),);
ANOLIS_FN(int anolis_main(), 0);
ANOLIS_FN(void * anolis_get_menu_builder(), 0);
ANOLIS_FN(int anolis_menu_builder_build_with_file(void *builder, char *filename, int def), 0);

