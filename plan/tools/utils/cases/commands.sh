#### printf as a command
env printf '%s-%d\n' a 1 b 2

#### echo as a command
env echo a 'b\tc'

#### test as a command
env test 1 -lt 2; echo "st=$?"; env test -z x; echo "st=$?"

#### true and false as commands
env true; echo "st=$?"; env false; echo "st=$?"

#### find -exec test
printf x > f1; : > f2; find . -name 'f*' -exec test -s {} \; -print | sort

#### xargs with echo
printf 'a\nb\n' | xargs echo
