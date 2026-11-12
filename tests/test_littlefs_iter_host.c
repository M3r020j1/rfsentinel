/* Real bundled LittleFS, RAM block device: mutation while a directory is open. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lfs.h"
#define BS 4096
#define BC 1024
static unsigned char storage[BS * BC];
static int bread(const struct lfs_config*c,lfs_block_t b,lfs_off_t o,void*p,lfs_size_t n){(void)c;memcpy(p,storage+b*BS+o,n);return 0;}
static int bprog(const struct lfs_config*c,lfs_block_t b,lfs_off_t o,const void*p,lfs_size_t n){(void)c;unsigned char*d=storage+b*BS+o;const unsigned char*s=p;for(lfs_size_t i=0;i<n;i++){assert((d[i]&s[i])==s[i]);d[i]&=s[i];}return 0;}
static int berase(const struct lfs_config*c,lfs_block_t b){(void)c;memset(storage+b*BS,0xff,BS);return 0;}
static int bsync(const struct lfs_config*c){(void)c;return 0;}
static void create(lfs_t*l,int i){char path[64];snprintf(path,sizeof(path),"/obs/obs_s%010d_N.csv",i);lfs_file_t f;assert(lfs_file_open(l,&f,path,LFS_O_WRONLY|LFS_O_CREAT|LFS_O_EXCL)==0);assert(lfs_file_write(l,&f,"data\n",5)==5);assert(lfs_file_close(l,&f)==0);}
int main(void){
 memset(storage,0xff,sizeof(storage));struct lfs_config c={.read=bread,.prog=bprog,.erase=berase,.sync=bsync,.read_size=16,.prog_size=16,.block_size=BS,.block_count=BC,.cache_size=256,.lookahead_size=32,.block_cycles=500};
 lfs_t l;assert(lfs_format(&l,&c)==0);assert(lfs_mount(&l,&c)==0);assert(lfs_mkdir(&l,"/obs")==0);
 for(int i=1;i<=600;i++)create(&l,i);
 lfs_dir_t d;assert(lfs_dir_open(&l,&d,"/obs")==0);int seen[601]={0},created=0,base=0,r;struct lfs_info info;
 while((r=lfs_dir_read(&l,&d,&info))>0){
  int n=0;char tail=0;if(sscanf(info.name,"obs_s%d_N.csv%c",&n,&tail)==1 && n>=1 && n<=600){seen[n]++;base++;}
  if(base>0 && base%3==0 && created<200){create(&l,601+created);created++;}
 }
 assert(r==0 && lfs_dir_close(&l,&d)==0);assert(base==600 && created>0);
 for(int i=1;i<=600;i++)assert(seen[i]==1);
 assert(lfs_unmount(&l)==0);printf("LittleFS: %d original entries seen once while %d new segments were created\n",base,created);return 0;
}
