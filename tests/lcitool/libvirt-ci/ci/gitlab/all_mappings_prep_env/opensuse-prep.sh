#!/bin/sh

zypper dist-upgrade -y
zypper install -y \
    perl-App-cpanminus \
    python313-base \
    python313-pip
test -f /usr/bin/python3 || ln -s /usr/bin/python3.13 /usr/bin/python3
