function install_buildenv() {
    dnf --quiet update -y
    dnf --quiet install 'dnf-command(config-manager)' -y
    dnf --quiet config-manager --set-enabled -y crb
    dnf --quiet install -y epel-release
    dnf --quiet install almalinux-release-devel -y
    dnf --quiet config-manager --set-enabled -y devel
    dnf --quiet install -y \
                ca-certificates \
                git \
                glibc-langpack-en \
                golang
    rpm -qa | sort > /packages.txt
}

export LANG="en_US.UTF-8"