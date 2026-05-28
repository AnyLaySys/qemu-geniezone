function install_buildenv() {
    dnf --quiet update -y --nogpgcheck fedora-gpg-keys
    dnf --quiet distro-sync -y
    dnf --quiet install -y \
                ca-certificates \
                ccache \
                gcc \
                git \
                glibc-devel \
                glibc-langpack-en \
                golang \
                pkgconfig
    rpm -qa | sort > /packages.txt
    mkdir -p /usr/libexec/ccache-wrappers
    ln -s /usr/bin/ccache /usr/libexec/ccache-wrappers/cc
    ln -s /usr/bin/ccache /usr/libexec/ccache-wrappers/gcc
}

export CCACHE_WRAPPERSDIR="/usr/libexec/ccache-wrappers"
export LANG="en_US.UTF-8"
