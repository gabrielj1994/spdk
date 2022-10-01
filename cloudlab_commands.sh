#===
#copy ssh key

#===
#~/init_script.sh

echo 1024 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
#cp /opt/GIT/SETUP_FILES/id_ed25519 ~/.ssh/id_ed25519
chmod 600 ~/.ssh/id_ed25519
eval "$(ssh-agent -s)"
ssh-add ~/.ssh/id_ed25519
ssh -vT git@github.com
export DPDK_ROOT=~/dpdk
export SPDK_ROOT=~/spdk

#===
#set up commands
mkdir ~/GIT
cd ~/GIT
git clone git@github.com:gabrielj1994/dpdk.git
git clone git@github.com:gabrielj1994/spdk.git
cp -r ~/GIT/dpdk ~/
cp -r ~/GIT/spdk ~/
cd ~/dpdk
git checkout releases
meson build
meson configure -Dprefix=$PWD/build build
ninja -C build
ninja -C build install
export DPDK_ROOT=~/dpdk
export SPDK_ROOT=~/spdk
cd ~/spdk
git checkout v22.05.x
./configure --with-dpdk=${DPDK_ROOT}/build/
git submodule update --init
./configure --with-dpdk=${DPDK_ROOT}/build/
make -j 48


sudo PCI_BLOCKED=0000:c5:00.0 ./scripts/setup.sh

#===
#~/update_lab2.sh
cd ~/GIT/spdk/
git checkout Lab2
git pull
cp ~/GIT/spdk/hello_world/hello_world.c ~/hello_world/hello_world.c
cd ~/hello_world
make build
cp ~/GIT/spdk/skeleton/main.c ~/skeleton/main.c
cd ~/skeleton
make build
cp ~/GIT/spdk/skeleton_client/main.c ~/skeleton_client/main.c
cd ~/skeleton_client
make build