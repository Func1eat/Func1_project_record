#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

# image directory
IMGDIR=$HOME/images
# Virtual machine disk image
OSIMGF=$IMGDIR/femu.qcow2

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=32 # number of sectors in a flash page
pgs_per_blk=1024 # number of pages per flash block
blks_per_pl=512 # number of blocks per plane
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=2 # number of chips per channel
nchs=2 # number of channels
ssd_size=22937 # in megabytes, if you change the above layout parameters, make sure you manually recalculate the ssd size and modify it here, please consider a default 25% overprovisioning ratio.

# Latency in nanoseconds
pg_rd_lat=40000 # page read latency
pg_wr_lat=200000 # page write latency
blk_er_lat=2000000 # block erase latency
ch_xfer_lat=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=70
gc_thres_pcent_high=90

cv_enabled=1
fdp_enabled=1
read_migration=0 #0表示不迁移，1表示根据读取次数做迁移，2表示根据页面类型和读取次数做迁移
ru_mode=0
write_mode=1  # 0表示全写入slc，1表示根据写热度 2表示根据综合热度 3表示根据大小
wl_mode=2
enable_cap_loss=51

# slc qlc比例
slc_op=10
qlc_op=90

util_ratio_low=30
util_ratio_high=80

# QLC区域中均衡块和不均衡块数量对比
balance_ratio=100
unbalance_ratio=0

# 读写buffer大小
write_buffer_capacity=4
read_buffer_capacity=0

# 读区域最大超级块数量
ra_max_cnt=3
wa_max_cnt=72

dynamic_ra_flag=1

dynamic_tw_flag=1

dynamic_tm_flag=0
#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",cv_enabled=${cv_enabled}"
FEMU_OPTIONS=${FEMU_OPTIONS}",fdp_enabled=${fdp_enabled}"
FEMU_OPTIONS=${FEMU_OPTIONS}",read_migration=${read_migration}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ru_mode=${ru_mode}"
FEMU_OPTIONS=${FEMU_OPTIONS}",write_mode=${write_mode}"
FEMU_OPTIONS=${FEMU_OPTIONS}",wl_mode=${wl_mode}"
FEMU_OPTIONS=${FEMU_OPTIONS}",enable_cap_loss=${enable_cap_loss}"
FEMU_OPTIONS=${FEMU_OPTIONS}",slc_op=${slc_op}"
FEMU_OPTIONS=${FEMU_OPTIONS}",qlc_op=${qlc_op}"
FEMU_OPTIONS=${FEMU_OPTIONS}",util_ratio_low=${util_ratio_low}"
FEMU_OPTIONS=${FEMU_OPTIONS}",util_ratio_high=${util_ratio_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",balance_ratio=${balance_ratio}"
FEMU_OPTIONS=${FEMU_OPTIONS}",unbalance_ratio=${unbalance_ratio}"
FEMU_OPTIONS=${FEMU_OPTIONS}",write_buffer_capacity=${write_buffer_capacity}"
FEMU_OPTIONS=${FEMU_OPTIONS}",read_buffer_capacity=${read_buffer_capacity}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ra_max_cnt=${ra_max_cnt}"
FEMU_OPTIONS=${FEMU_OPTIONS}",wa_max_cnt=${wa_max_cnt}"
FEMU_OPTIONS=${FEMU_OPTIONS}",dynamic_ra_flag=${dynamic_ra_flag}"
FEMU_OPTIONS=${FEMU_OPTIONS}",dynamic_tw_flag=${dynamic_tw_flag}"
FEMU_OPTIONS=${FEMU_OPTIONS}",dynamic_tm_flag=${dynamic_tm_flag}"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

sudo /home/huangkeyu/fdp_simulator/build-femu/qemu-system-x86_64 \
    -L /home/huangkeyu/fdp_simulator/build-femu/qemu-bundle/usr/local/share/qemu \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 4G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::6060-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log

