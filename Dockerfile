# misc-lab-4: Modem Control Lab -- everything builds and runs in here.
FROM debian:bookworm
# Plain-http Debian mirrors are blocked (403) on this network, so switch to https.
# The base image has no CA bundle yet, so the FIRST install skips TLS peer checks
# only to fetch ca-certificates; package integrity is still enforced by apt's GPG
# signature check on the Release files. After that, TLS verification is normal.
RUN sed -i 's|http://|https://|g' /etc/apt/sources.list.d/debian.sources \
 && apt-get -o Acquire::https::Verify-Peer=false update \
 && apt-get -o Acquire::https::Verify-Peer=false install -y --no-install-recommends ca-certificates \
 && apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config libgtest-dev \
      libqmi-glib-dev libmbim-glib-dev libqrtr-glib-dev libglib2.0-dev \
      iproute2 gdb strace \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
