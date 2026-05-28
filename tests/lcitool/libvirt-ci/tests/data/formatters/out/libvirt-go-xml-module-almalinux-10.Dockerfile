FROM docker.io/library/almalinux:10

RUN dnf --quiet update -y && \
    dnf --quiet install 'dnf-command(config-manager)' -y && \
    dnf --quiet config-manager --set-enabled -y crb && \
    dnf --quiet install -y epel-release && \
    dnf --quiet install almalinux-release-devel -y && \
    dnf --quiet config-manager --set-enabled -y devel && \
    dnf --quiet install -y \
                ca-certificates \
                git \
                glibc-langpack-en \
                golang && \
    dnf --quiet autoremove -y && \
    dnf --quiet clean all -y && \
    rpm -qa | sort > /packages.txt

ENV LANG="en_US.UTF-8"