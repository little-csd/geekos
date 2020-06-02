#include <stdio.h>
#include <string.h>
#include <stdlib.h>

char*
connectName(char* prefix, char* path) {
    if (!path || !prefix) return 0;
    char* buf;
    if (path[0] == '/') {
        int len = strlen(path);
        buf = (char*)malloc(len+1);
        memcpy(buf, path, len);
        buf[len] = '\0';
        return buf;
    } else {
        int len1 = strlen(prefix);
        int len2 = strlen(path);
        if (prefix[len1-1] == '/') {
            int len = len1 + len2;
            buf = (char*)malloc(len+1);
            memcpy(buf, prefix, len1);
            memcpy(buf+len1, path, len2);
            buf[len] = '\0';
        } else {
            int len = len1 + len2 + 1;
            buf = (char*)malloc(len+1);
            memcpy(buf, prefix ,len1);
            buf[len1] = '/';
            memcpy(buf+len1+1, path, len2);
            buf[len] = '\0';
        }
    }
    return buf;
}
int main() {
	char buf1[10] = "c/";
	char buf2[10] = "d/file.c";
	char* buf = connectName(buf1,buf2);
	printf("%s\n", buf);
	return 0;
}
