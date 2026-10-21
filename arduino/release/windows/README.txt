JieLi toolchain for the SMC-PAD Arduino platform, Windows

This folder fills itself the first time a sketch is built: jieli_fetch.cmd downloads JieLi's
toolchain installer (2.5.3.exe, from JieLi's server, checked against its SHA-256) and extracts
only the compiler (the pi32 folder) here with innoextract -- nothing is installed, nothing is
written outside this folder, CodeBlocks is not needed. Removing the platform removes it.

innoextract (https://constexpr.org/innoextract/, zlib license, see innoextract-LICENSE.txt) is
included unchanged.
