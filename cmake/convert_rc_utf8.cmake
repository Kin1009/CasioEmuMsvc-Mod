# Convert the UTF-16LE Visual Studio .rc file into UTF-8 for MinGW windres.
# Called with -DINPUT=<src> -DOUTPUT=<dst>.
execute_process(
    COMMAND iconv -f UTF-16LE -t UTF-8 "${INPUT}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "iconv failed to convert ${INPUT}: rc=${_rc}")
endif()
