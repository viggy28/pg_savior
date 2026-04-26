ARG PG_MAJOR=16
FROM postgres:${PG_MAJOR}

ARG PG_MAJOR
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
         build-essential \
         postgresql-server-dev-${PG_MAJOR} \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /pg_savior
COPY . /pg_savior

RUN make clean && make && make install
