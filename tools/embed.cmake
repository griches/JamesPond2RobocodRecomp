# Turns a file into a C++ array: cmake -DIN=file -DOUT=out.cpp -DNAME=symbol -P embed.cmake
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" chars)
math(EXPR bytes "${chars} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," body "${hex}")
file(WRITE ${OUT} "// Generated from ${IN}\nextern const unsigned char ${NAME}[] = {${body}};\nextern const unsigned int ${NAME}_size = ${bytes};\n")
