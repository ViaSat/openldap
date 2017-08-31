FROM ubuntu:14.04
MAINTAINER ViaSat Cloud Engineering <vice@viasat.com>

RUN apt-get update \
    && apt-get install --no-install-recommends --no-install-suggests -y \
        build-essential autoconf automake autotools-dev debhelper dh-make \
        devscripts fakeroot file gnupg git lintian patch patchutils pbuilder \
        ca-certificates

RUN apt-get update \
    && apt-get install --no-install-recommends --no-install-suggests -y \
        libltdl7 libltdl-dev groff-base libssl-dev libdb-dev zlib1g zlib1g-dev \
        libwrap0-dev libsasl2-2 libsasl2-dev openssl mime-support mawk \
        libcrack2-dev libslp1 libwrap0-dev

RUN git clone https://github.com/ltb-project/openldap-deb /opt/

COPY . /opt/debian/paquet-openldap-debian/openldap-ltb-2.4.45

RUN cd /opt/debian/paquet-openldap-debian/openldap-ltb-2.4.45 \
    && ./configure --disable-dependency-tracking \
        --prefix=/usr/local/openldap --libdir=/usr/local/openldap/${_LIB} \
        --enable-hdb --enable-bdb --enable-overlays --enable-modules \
        --enable-dynamic --with-tls=openssl --enable-debug --with-cyrus-sasl \
        --enable-spasswd --enable-ppolicy --enable-crypt --enable-ldap \
        --enable-slapi --enable-meta --enable-sock --enable-wrappers \
    && make depend \
    && make "prefix=/usr/local/openldap" \
    && cd contrib/slapd-modules/lastbind \
    && make "prefix=/usr/local/openldap"

# ENV DEBEMAIL="vice@viasat.com"
# ENV DEBFULLNAME="ViaSat Cloud Engineering"

# RUN cd /opt/debian/paquet-openldap-debian/openldap-ltb-2.4.45 \
#     && sed -i.bak "s/lastbind/lastbind2/" debian/postbuild \
#     && debian/rules clean \
#     && debian/rules build \
#     && debian/rules binary

# RUN cd ~ \
#     && wget https://ltb-project.org/archives/berkeleydb-ltb-4.6.21-jessie-64.tar.gz \
#     && tar -xzf berkeleydb-ltb-4.6.21-jessie-64.tar.gz \
#     && dpkg -i berkeleydb-ltb_4.6.21.NC-4-patch4_amd64.deb \
#     && cd /opt/debian/paquet-openldap-debian \
#     && dpkg -i openldap-ltb_2.4.45.1_amd64.deb \
#     && dpkg -i openldap-ltb-contrib-overlays_2.4.45.1_amd64.deb

