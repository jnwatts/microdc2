#ifndef __TTH_FILE_H
#define __TTH_FILE_H

#define IS_CURRENT_DIR(x) ((x)[0] == '.' && (x)[1] == '\0')
#define IS_PARENT_DIR(x)  ((x)[0] == '.' && (x)[1] == '.' && (x)[2] == '\0')
#define IS_SPECIAL_DIR(x) (IS_CURRENT_DIR(x) || IS_PARENT_DIR(x) || strncmp(x, tth_directory_name, strlen(tth_directory_name)) == 0)

extern const char tth_directory_name[];

#endif // ifndef __TTH_FILE_H
