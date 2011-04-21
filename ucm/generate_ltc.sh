#!/bin/bash

# The script below uses the following sed script:
# '/\\$/{$ s/\\$//;$! H};/\\$/!{H;g;s/[[:space:]]+\\\n[[:space:]]+/ /g;s/^\n//;p;z;h}'
# Unwrap lines with a trailing backslash. This works as follows:
# If the line ends in a backslash, there are two cases:
# - if the line is the last line in the file, just remove the backslash [forcing
#   a fallthrough to case without trailing backslash]
# - if the line is not the last in the file, append to hold space
# If the line does not end in a backslash:
# - append to hold space
# - copy hold space to pattern space
# - replace all trailing backslashes by a single space
# - remove starting backslash introduced by H commands
# - print output
# - clear pattern space
# - copy pattern space to hold space
# The last two commands are necessary because there is no command to clear the
# hold space.

unset HANDLED
while read TARGET FILES ; do
	echo "Generating ../src/tables/${TARGET%:}.c"
	../src.util/ucm2ltc -o "../src/tables/${TARGET%:}.c" $FILES
	for f in $FILES ; do
		if [ "x${f#-}" != "x$f" ] ; then
			continue
		fi
		HANDLED="$HANDLED$f"$'\n'
	done
done < <(sed -r -n '/\\$/{$ s/\\$//;$! H};/\\$/!{H;g;s/[[:space:]]+\\\n[[:space:]]+/ /g;s/^\n//;p;z;h}' rules | \
	sed -r '/^[[:space:]]*$/d')

for f in `{ echo "$HANDLED" ; find -name '*.ucm' -printf '%P\n' ; } | sort | uniq -u` ; do
	out="${f##*/}"
	echo "Generating ../src/tables/${out%.ucm}.c"
	../src.util/ucm2ltc -o "../src/tables/${out%.ucm}.c" $f
done
