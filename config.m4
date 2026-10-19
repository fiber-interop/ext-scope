PHP_ARG_ENABLE([scope],
  [whether to enable the Scope class],
  [AS_HELP_STRING([--enable-scope], [Enable the Scope class])],
  [yes])

if test "$PHP_SCOPE" != "no"; then
  PHP_NEW_EXTENSION(scope, scope.c, $ext_shared)
  PHP_ADD_MAKEFILE_FRAGMENT
fi
