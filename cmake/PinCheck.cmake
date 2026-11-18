# Warns when DONOR_REV or DAF_REV is behind what the two repositories have
# released, as scripts/check-pins.sh finds it. Included at configure time and
# run by the duskstudio-pin-check target on every build; between its fetches the
# script answers from its cache, so a build pays for the network twice a day,
# and a fetch that hangs is given up on after half a minute.
#
# Wants DUSKSTUDIO_PIN_BASH, DUSKSTUDIO_PIN_SOURCE_DIR and DUSKSTUDIO_PIN_CACHE_DIR.

execute_process(
    COMMAND "${DUSKSTUDIO_PIN_BASH}" "${DUSKSTUDIO_PIN_SOURCE_DIR}/scripts/check-pins.sh"
            --cache-dir "${DUSKSTUDIO_PIN_CACHE_DIR}" --max-age-hours 12
    OUTPUT_VARIABLE _duskstudio_pin_lines
    RESULT_VARIABLE _duskstudio_pin_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    TIMEOUT 30)
if(_duskstudio_pin_result EQUAL 1)
    message(WARNING
        "${_duskstudio_pin_lines}\n"
        "Dusk Studio is building older plug-in or framework code than has been "
        "released. Moving the pins is described under \"Moving the donor and DAF "
        "pins\" in docs/MAINTAINER-GUIDE.md.")
endif()
