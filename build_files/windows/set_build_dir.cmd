if NOT "%TARGET%" == "" (
    set BUILD_DIR=%BUILD_DIR%_%TARGET%_%BUILD_ARCH%_vc%BUILD_VS_VER%_%BUILD_TYPE%
) else (
    set BUILD_DIR=%BUILD_DIR%_%BUILD_ARCH%_vc%BUILD_VS_VER%_%BUILD_TYPE%
)

if NOT "%BUILD_DIR_OVERRRIDE%"=="" (
	set BUILD_DIR=%BUILD_DIR_OVERRRIDE%
)