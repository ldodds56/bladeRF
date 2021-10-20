Setup:

Install Quartus Lite 19.1 (same version used by BladeRF to generate v0.12.0)
Extract .tar, run /.setup.sh (no sudo)

Add the following to your ~/.bashrc:
export PATH=$PATH:/home/$USER/intelFPGA_lite/19.1/nios2eds/bin/gnu/H-x86_64-pc-linux-gnu/bin:/home/$USER/intelFPGA_lite/19.1/nios2eds/sdk2/bin:/home/$USER/intelFPGA_lite/19.1/nios2eds/bin:/home/$USER/intelFPGA_lite/19.1/modelsim_ase/linuxaloem:/home/$USER/intelFPGA_lite/19.1/quartus/bin:/home/$USER/intelFPGA_lite/19.1/quartus/sopc_builder/bin:/home/$USER/intelFPGA_lite/19.1/quartus/bin:/home/$USER/intelFPGA_lite/19.1/quartus/sopc_builder/bin:/home/$USER/intelFPGA_lite/19.1/nios2eds/sdk2/bin
export QUARTUS_ROOTDIR=/home/$USER/intelFPGA_lite/19.1/quartus
export SOPC_KIT_NIOS2=/home/$USER/intelFPGA_lite/19.1/nios2eds
export WSLENV=$QUARTUS_ROOTDIR/p:$SOPC_KIT_NIOS2/p

Building FPGA:
cd ~/bladerf_dev/bladeRF/hdl/quartus
./build_bladerf.sh -b bladeRF-micro -s A4 -r hosted


Issues:
1) make: *** No rule to make target '../../../../../../thirdparty/analogdevicesinc/no-OS/ad9361/sw/*', needed by '../../../../../quartus/work/bladerf-micro-A4-hosted/libad936x/Makefile'.  Stop.

Make sure you checked out submodules for this repo
git submodule init
git submodule update --recursive
