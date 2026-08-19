/* Stub for the miniz_export.h upstream CMake builds generate via
 * GenerateExportHeader. mino compiles miniz as plain C with default
 * visibility, so every export macro is empty. Guarded so a builder
 * supplying a real export header wins. */
#ifndef MINIZ_MINIZ_EXPORT_H
#define MINIZ_MINIZ_EXPORT_H

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif

#ifndef MINIZ_NO_EXPORT
#define MINIZ_NO_EXPORT
#endif

#ifndef MINIZ_DEPRECATED
#define MINIZ_DEPRECATED
#endif

#endif
