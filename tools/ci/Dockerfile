# SPDX-License-Identifier: GPL-2.0-or-later
#
# The CI environment for tools/ci-local.sh: Ubuntu 24.04 with the tools
# GitHub's ubuntu-24.04 runner image provides (gcc 13, clang 18, Python 3.12,
# git) and the packages .github/workflows/ci.yml installs (cppcheck 2.13,
# the libFuzzer runtime, the toolchain's build prerequisites), and ccache,
# which only the local runs use.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -q && apt-get install -y -q --no-install-recommends \
		build-essential clang python3 git ca-certificates file tzdata \
		cppcheck \
		libclang-rt-18-dev \
		libgmp-dev libmpfr-dev libmpc-dev flex bison \
		ccache \
	&& update-ccache-symlinks \
	&& rm -rf /var/lib/apt/lists/*

# The jobs run as an unprivileged user, as on the runners (uid 1001). The
# toolchain and ccache volumes are mounted on /cache and /ccache and take
# these directories' owner when they are first created.
RUN useradd -m -u 1001 runner && mkdir /cache /ccache && chown runner: /cache /ccache
USER runner
ENV WAFFLE_TOOLCHAIN_CACHE=/cache CCACHE_DIR=/ccache
WORKDIR /home/runner
