/* Neptune configuration for the vendored TinyCC sources. */
#ifndef NEPTUNE_TCC_CONFIG_H
#define NEPTUNE_TCC_CONFIG_H

#define TCC_VERSION "0.9.28rc-neptune"
#define TCC_TARGET_ARM64 1
#define CONFIG_TCC_STATIC 1
#define CONFIG_TCC_PREDEFS 1
#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCCDIR "/tcc"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#define CONFIG_TCC_LIBPATHS "{B}/lib"
#define CONFIG_TCC_CRTPREFIX "/tcc/lib"
#define CONFIG_TCC_ELFINTERP ""

#endif
