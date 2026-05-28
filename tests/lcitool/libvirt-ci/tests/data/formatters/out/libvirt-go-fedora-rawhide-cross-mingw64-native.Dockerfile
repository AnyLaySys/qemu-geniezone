FROM registry.fedoraproject.org/fedora:rawhide

RUN dnf --quiet update -y --nogpgcheck fedora-gpg-keys && \
    dnf --quiet install -y nosync && \
    printf '#!/bin/sh\n\
if test -d /usr/lib64\n\
then\n\
    export LD_PRELOAD=/usr/lib64/nosync/nosync.so\n\
else\n\
    export LD_PRELOAD=/usr/lib/nosync/nosync.so\n\
fi\n\
exec "$@"\n' > /usr/bin/nosync && \
    chmod +x /usr/bin/nosync && \
    nosync dnf --quiet distro-sync -y && \
    nosync dnf --quiet install -y \
                       ca-certificates \
                       ccache \
                       git \
                       glibc-langpack-en \
                       golang && \
    nosync dnf --quiet autoremove -y && \
    nosync dnf --quiet clean all -y

ENV CCACHE_WRAPPERSDIR="/usr/libexec/ccache-wrappers"
ENV LANG="en_US.UTF-8"