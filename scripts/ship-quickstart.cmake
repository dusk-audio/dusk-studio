# Write the copy of QUICKSTART.md that ships inside a package.
#
#   cmake -DQUICKSTART_IN=QUICKSTART.md -DQUICKSTART_OUT=<file> \
#         -DQUICKSTART_VERSION=X.Y.Z -P scripts/ship-quickstart.cmake
#
# The repository copy links the manual and its screenshots by relative path,
# and no package carries those files. The shipped copy points each relative
# link at the tagged source on GitHub instead; images go to the raw file so a
# Markdown viewer still renders them. URLs, rooted paths and in-page anchors
# are untouched.

foreach(var QUICKSTART_IN QUICKSTART_OUT QUICKSTART_VERSION)
    if("${${var}}" STREQUAL "")
        message(FATAL_ERROR "ship-quickstart.cmake: -D${var}= is required")
    endif()
endforeach()

file(READ "${QUICKSTART_IN}" text)
set(repo "dusk-audio/dusk-studio")
string(REGEX REPLACE "\\]\\(([^):#/][^):]*\\.(png|jpg|jpeg|gif|svg))\\)"
       "](https://raw.githubusercontent.com/${repo}/v${QUICKSTART_VERSION}/\\1)"
       text "${text}")
string(REGEX REPLACE "\\]\\(([^):#/][^):]*)\\)"
       "](https://github.com/${repo}/blob/v${QUICKSTART_VERSION}/\\1)"
       text "${text}")
file(WRITE "${QUICKSTART_OUT}" "${text}")
