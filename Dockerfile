# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION &
# AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
FROM ubuntu:22.04

ARG username

ENV TERM=linux
ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i -e 's/http:\/\/archive\.ubuntu\.com\/ubuntu\//http:\/\/us.archive.ubuntu.com\/ubuntu/' /etc/apt/sources.list

RUN apt-get update
RUN apt-get install -y
RUN apt-get install -y build-essential binutils-dev git-lfs wget

CMD ["/bin/bash"]
