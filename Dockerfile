FROM ubuntu:18.04

MAINTAINER Hirochika Asai <panda@jar.jp>

## Install build-essential and fuse
RUN apt-get update
RUN apt-get install -y --no-install-recommends build-essential fuse libfuse-dev vim-common automake autoconf pkg-config

COPY src /usr/src
WORKDIR /usr/src
RUN ./autogen.sh
RUN ./configure
RUN make clean all

## Execute bash
CMD ["./advfs", "/mnt", "-f"]

