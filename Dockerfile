FROM ubuntu:20.04

# Stop ubuntu-20 interactive options.
ENV DEBIAN_FRONTEND noninteractive

# Stop script if any individual command fails.
RUN set -e

# Define LLVM version.
ENV llvm_version=13.0.0

# Define home directory
ENV HOME=/app

# Define dependencies.
ENV lib_deps="make g++-8 gcc-8 git zlib1g-dev libncurses5-dev build-essential libssl-dev libpcre2-dev zip vim"
ENV build_deps="wget xz-utils cmake python git gdb tcl"

# Fetch dependencies.
RUN apt-get update --fix-missing
RUN apt-get install -y $build_deps $lib_deps

# Fetch and build SVF source.
WORKDIR ${HOME}
RUN git clone "https://oauth2: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@github.com/Northlake-Lab/COLLATE.git"
WORKDIR ${HOME}/COLLATE
RUN git submodule update --init
RUN echo "Building SVF ..."
WORKDIR ${HOME}/COLLATE/SVF
RUN bash ./build.sh

# Export SVF, llvm, z3 paths
ENV PATH=${HOME}/COLLATE/SVF/Release-build/bin:$PATH
ENV PATH=${HOME}/COLLATE/SVF/llvm-$llvm_version.obj/bin:$PATH
ENV SVF_DIR=${HOME}/COLLATE/SVF
ENV LLVM_DIR=${HOME}/COLLATE/SVF/llvm-$llvm_version.obj
ENV Z3_DIR=${HOME}/COLLATE/SVF/z3.obj

# build COLLATE
RUN echo "Building COLLATE ..."
WORKDIR ${HOME}/COLLATE
RUN bash ./build.sh

# Export COLLATE paths
ENV PATH=${HOME}/COLLATE/build/bin:$PATH
ENV COLLATE_DIR=${HOME}/COLLATE
