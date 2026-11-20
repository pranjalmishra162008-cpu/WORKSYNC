FROM debian:bookworm-slim

# Compiler + standard C headers
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY src/myweb.c ./myweb.c

# Values supplied by the hosting platform at build time (letters and digits only)
ARG WORKSYNC_SECRET
ARG PUBLIC_HOST

# Stop the build if the secret is missing, so we never ship the public default
RUN test -n "$WORKSYNC_SECRET" || (echo "ERROR: WORKSYNC_SECRET is not set" && exit 1)

# Patch the COPY inside the image only (your myweb.c in GitHub is never changed)
RUN sed -i "s|\"change-this-secret\"|\"${WORKSYNC_SECRET}\"|" myweb.c \
 && if [ -n "$PUBLIC_HOST" ]; then sed -i "s|harmless-suitably-glider.ngrok-free.dev|${PUBLIC_HOST}|" myweb.c; fi \
 && gcc -O2 -o worksync myweb.c

# Indian time instead of UTC (POSIX format: 5h30m ahead of UTC)
ENV TZ=IST-5:30

CMD ["./worksync"]
