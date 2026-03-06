#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define PORT 9999
#define MAGIC 0x50484F58
#define MAX_PACKET 10485760
#define FMT_H264 2


int recv_all(int sock,uint8_t *buf,int size)
{
    int total=0;

    while(total<size)
    {
        int n=recv(sock,buf+total,size-total,0);
        if(n<=0) return -1;
        total+=n;
    }

    return total;
}


int main()
{
    printf("Phoenix Receiver\n");

    int server=socket(AF_INET,SOCK_STREAM,0);

    int opt=1;
    setsockopt(server,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));

    addr.sin_family=AF_INET;
    addr.sin_port=htons(PORT);
    addr.sin_addr.s_addr=INADDR_ANY;

    bind(server,(struct sockaddr*)&addr,sizeof(addr));
    listen(server,1);

    printf("Listening on %d\n",PORT);

    int sock=accept(server,NULL,NULL);

    printf("Client connected\n");

    setsockopt(sock,IPPROTO_TCP,TCP_NODELAY,&opt,sizeof(opt));


    Display *display=XOpenDisplay(NULL);
    int screen=DefaultScreen(display);
    Window root=DefaultRootWindow(display);

    Window win=XCreateSimpleWindow(
        display,root,
        0,0,
        800,600,
        1,
        BlackPixel(display,screen),
        WhitePixel(display,screen)
    );

    XMapWindow(display,win);

    GC gc=XCreateGC(display,win,0,NULL);


    uint8_t *packet=malloc(MAX_PACKET);

    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *codec_ctx=avcodec_alloc_context3(codec);
    avcodec_open2(codec_ctx,codec,NULL);

    AVFrame *frame=av_frame_alloc();
    AVPacket *pkt=av_packet_alloc();

    struct SwsContext *sws=NULL;

    uint8_t *rgb_buffer=NULL;

    int last_w=0;
    int last_h=0;

    XImage *img=NULL;
    XShmSegmentInfo shminfo;


    while(1)
    {
        uint32_t header[5];

        if(recv_all(sock,(uint8_t*)header,sizeof(header))<0)
            break;

        uint32_t magic=ntohl(header[0]);
        uint32_t w=ntohl(header[1]);
        uint32_t h=ntohl(header[2]);
        uint32_t fmt=ntohl(header[3]);
        uint32_t size=ntohl(header[4]);

        if(magic!=MAGIC) break;
        if(size>MAX_PACKET) break;

        if(recv_all(sock,packet,size)<0)
            break;

        if(fmt!=FMT_H264)
            continue;

        pkt->data=packet;
        pkt->size=size;

        int ret=avcodec_send_packet(codec_ctx,pkt);

        if(ret<0) continue;

        while(ret>=0)
        {
            ret=avcodec_receive_frame(codec_ctx,frame);

            if(ret==AVERROR(EAGAIN)||ret==AVERROR_EOF)
                break;

            if(ret<0) break;


            if(!sws || frame->width!=last_w || frame->height!=last_h)
            {
                if(sws) sws_freeContext(sws);

                sws=sws_getContext(
                    frame->width,
                    frame->height,
                    frame->format,
                    frame->width,
                    frame->height,
                    AV_PIX_FMT_BGR24,
                    SWS_FAST_BILINEAR,
                    NULL,NULL,NULL
                );
            }


            int num=av_image_get_buffer_size(
                AV_PIX_FMT_BGR24,
                frame->width,
                frame->height,
                1
            );

            rgb_buffer=realloc(rgb_buffer,num);

            uint8_t *dst[4]={rgb_buffer,NULL,NULL,NULL};
            int lines[4]={frame->width*3,0,0,0};

            sws_scale(
                sws,
                (const uint8_t * const*)frame->data,
                frame->linesize,
                0,
                frame->height,
                dst,
                lines
            );


            w=frame->width;
            h=frame->height;

            if(w!=last_w || h!=last_h)
            {
                XResizeWindow(display,win,w,h);

                if(img)
                {
                    XShmDetach(display,&shminfo);
                    XDestroyImage(img);
                    shmdt(shminfo.shmaddr);
                    shmctl(shminfo.shmid,IPC_RMID,0);
                }

                img=XShmCreateImage(
                    display,
                    DefaultVisual(display,screen),
                    24,
                    ZPixmap,
                    NULL,
                    &shminfo,
                    w,h
                );

                shminfo.shmid=shmget(
                    IPC_PRIVATE,
                    img->bytes_per_line*img->height,
                    IPC_CREAT|0777
                );

                shminfo.shmaddr=img->data=shmat(shminfo.shmid,0,0);
                shminfo.readOnly=False;

                XShmAttach(display,&shminfo);

                last_w=w;
                last_h=h;
            }


            uint8_t *dstptr=(uint8_t*)img->data;

            for(int i=0;i<w*h;i++)
            {
                dstptr[i*4+0]=rgb_buffer[i*3+0];
                dstptr[i*4+1]=rgb_buffer[i*3+1];
                dstptr[i*4+2]=rgb_buffer[i*3+2];
                dstptr[i*4+3]=0;
            }


            XShmPutImage(
                display,
                win,
                gc,
                img,
                0,0,
                0,0,
                w,h,
                False
            );

            XFlush(display);
        }

        av_packet_unref(pkt);
    }

    return 0;
}
