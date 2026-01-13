SUMMARY = "OpenBMC template application"
DESCRIPTION = "The OpenBMC template application is designed to be used as a starting point for developing new OpenBMC applications"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"
DEPENDS = " \
  sdbusplus \
  ${@bb.utils.contains('PTEST_ENABLED', '1', 'gtest', '', d)} \
  ${@bb.utils.contains('PTEST_ENABLED', '1', 'gmock', '', d)} \
"
SRC_URI = "git://github.com/edtanous/openbmc_template_app;branch=master;protocol=https"
SRCREV = "243e128acb6cd07e67b0d6b7458bedf5c4be25af"
PV = "1.0+git${SRCPV}"
SYSTEMD_SERVICE:${PN}:append = "xyz.openbmc_project.templateapp.service"
S = "${WORKDIR}/git"
inherit meson systemd pkgconfig

PACKAGECONFIG ??= ""

EXTRA_OEMESON = " \
    -Dtests=${@bb.utils.contains('PTEST_ENABLED', '1', 'enabled', 'disabled', d)} \
"

do_install_ptest() {
    install -d ${D}${PTEST_PATH}/test
    cp -rf ${B}/*_test ${D}${PTEST_PATH}/test/
}
