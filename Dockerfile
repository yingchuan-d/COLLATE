FROM ubuntu:20.04

# Stop ubuntu-20 interactive options.
ENV DEBIAN_FRONTEND noninteractive

# Stop script if any individual command fails.
RUN set -e

# Define LLVM version.
ENV llvm_version=13.0.0

# Define home directory
ENV HOME=/home/COLLATE

# Define dependencies.
ENV lib_deps="make g++-8 gcc-8 git zlib1g-dev libncurses5-dev build-essential libssl-dev libpcre2-dev zip vim"
ENV build_deps="wget xz-utils cmake python git gdb tcl"

# Fetch dependencies.
RUN apt-get update --fix-missing
RUN apt-get install -y $build_deps $lib_deps

# Fetch and build COLLATE
RUN echo "Downloading LLVM and building COLLATE to " ${HOME}
WORKDIR ${HOME}
RUN git clone "https://github.com/Northlake-Lab/COLLATE.git"
WORKDIR ${HOME}/COLLATE
RUN echo "Building COLLATE ..."
RUN bash ./build.sh
