# docker build -t vkb-arch-gamescope - < host/devprofile/gamescope.Dockerfile (devprofile_check.sh, run_tests.sh)
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed gamescope vulkan-tools mesa-utils mesa vulkan-icd-loader && pacman -Scc --noconfirm
