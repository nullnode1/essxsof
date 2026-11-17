/* SPDX-License-Identifier: BSD-2-Clause */
/* Ordinary OSS writer; no private driver tone interface. */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static const int32_t sine48[48]={
    0,68433,135695,200636,262144,319177,370728,415965,
    454047,484379,506423,519803,524287,519803,506423,484379,
    454047,415965,370728,319177,262144,200636,135695,68433,
    0,-68433,-135695,-200636,-262144,-319177,-370728,-415965,
    -454047,-484379,-506423,-519803,-524287,-519803,-506423,-484379,
    -454047,-415965,-370728,-319177,-262144,-200636,-135695,-68433};

int main(int argc,char **argv)
{
    uint8_t block[6144];
    uint32_t frames=0,n,i,v;
    size_t bytes,done;
    int fd,format=AFMT_S32_LE,channels=2,rate=48000;
    ssize_t wrote;
    if(argc!=2 || strncmp(argv[1],"/dev/dsp",8)!=0) {
        fprintf(stderr,"Kullanim: %s /dev/dspN\n",argv[0]);return(2);
    }
    alarm(15);
    fd=open(argv[1],O_WRONLY);
    if(fd<0) {perror("PCM open");return(1);}
    errno=0;
    if(ioctl(fd,SNDCTL_DSP_SETFMT,&format)<0 || format!=AFMT_S32_LE ||
        ioctl(fd,SNDCTL_DSP_CHANNELS,&channels)<0 || channels!=2 ||
        ioctl(fd,SNDCTL_DSP_SPEED,&rate)<0 || rate!=48000) {
        fprintf(stderr,"PCM ayarlanamadi: fmt=0x%x channels=%d rate=%d errno=%d\n",
            format,channels,rate,errno);
        close(fd);return(1);
    }
    printf("%s: normal PCM write(), 1 kHz, 2 saniye\n",argv[1]);fflush(stdout);
    while(frames<96000) {
        n=96000-frames;if(n>768)n=768;
        for(i=0;i<n;i++) {
            v=(uint32_t)(sine48[(frames+i)%48]*1024);
            for(unsigned ch=0;ch<2;ch++) {
                uint8_t *b=block+i*8+ch*4;
                b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);
                b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);
            }
        }
        bytes=(size_t)n*8;done=0;
        while(done<bytes) {
            wrote=write(fd,block+done,bytes-done);
            if(wrote<0 && errno==EINTR)continue;
            if(wrote<=0) {perror("PCM write");close(fd);return(1);}
            done+=(size_t)wrote;
        }
        frames+=n;
    }
    if(ioctl(fd,SNDCTL_DSP_SYNC,NULL)<0) {perror("PCM sync");close(fd);return(1);}
    close(fd);alarm(0);puts("PCM yazma ve drain tamamlandi.");return(0);
}
