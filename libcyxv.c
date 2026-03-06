#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int engine_started = 0; // 點火狀態標記

void recv_all(int sock, unsigned char *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n = recv(sock, buf + total, size - total, 0);
        if (n <= 0) break;
        total += n;
    }
}

// 🕷️ 幽靈伺服器 (在背景獨立運作，不干擾 X Server 主線程)
void* phoenix_server(void* arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(9999);
    
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 5);
    
    // 現在我們是自由的，可以隨便呼叫 XOpenDisplay
    Display *d = XOpenDisplay(":0");
    if (!d) return NULL;

    while(1) {
        int sock = accept(server_fd, NULL, NULL);
        if (sock < 0) continue;
        
        uint32_t header[3];
        recv_all(sock, (unsigned char*)header, 12);
        uint32_t win = header[0];
        uint32_t w = header[1];
        uint32_t h = header[2];
        
        size_t size = w * h * 4;
        unsigned char* pixels = malloc(size);
        recv_all(sock, pixels, size);
        
        if (win != 0 && w > 0 && h > 0) {
            GC gc = XCreateGC(d, win, 0, NULL);
            XImage *img = XCreateImage(d, DefaultVisual(d, 0), DefaultDepth(d, 0), 
                                       ZPixmap, 0, (char*)pixels, w, h, 32, 0);
            if (img) {
                XPutImage(d, win, gc, img, 0, 0, 0, 0, w, h);
                XFlush(d);
                img->data = NULL; // 斷開記憶體，防止崩潰
                XDestroyImage(img);
            }
            XFreeGC(d, gc);
        }
        free(pixels);
        close(sock);
    }
    return NULL;
}

// 🌟 點火開關：只在第一次被點擊時觸發
int FakeXvDispatch(void *client) {
    if (!engine_started) {
        engine_started = 1;
        pthread_t tid;
        pthread_create(&tid, NULL, phoenix_server, NULL);
        pthread_detach(tid);
    }
    // 立刻返回，絕不卡死 X Server 總機！
    return 0; 
}
