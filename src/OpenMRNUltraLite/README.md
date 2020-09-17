# OpenMRNUltraLite

This is a stripped down version of the OpenMRNLite library that includes only
the required dependencies of the HttpServer. If the HttpServer library is being
used in a project that also includes the OpenMRN/OpenMRNLite library the
configuration should be setup to have HTTP_DNS_LIGHT_OPENMRN_LIB=n in the
sdkconfig.defaults file.