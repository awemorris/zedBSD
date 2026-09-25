#### rm -r removes a tree
mkdir -p d/e/f; touch d/a d/e/b d/e/f/c; rm -r d; ls

#### rm -rf with a missing operand is quiet
mkdir -p d/e; rm -rf d nosuch; echo "st=$?"; ls

#### rm -R is rm -r
mkdir -p d/e; touch d/e/x; rm -R d; ls

#### rm of a directory without -r fails
## skip-status
mkdir d; rm d 2>/dev/null; echo "st=$?"; ls

#### rm -f of a missing file succeeds
rm -f nosuch; echo "st=$?"

#### rm of a missing file fails
## skip-status
rm nosuch 2>/dev/null; echo "failed=$?"

#### rm -r of a dotted directory keeps . and ..
mkdir -p d/.hidden; touch d/.hidden/x d/.y; rm -r d; ls -a

#### rm -rf conftest style
mkdir conftest.dir; touch conftest.c conftest.o conftest.dir/x; rm -rf conftest*; ls

#### rmdir -p removes the parents
mkdir -p a/b/c; rmdir -p a/b/c; ls

#### mkdir -p is quiet for an existing directory
mkdir -p a/b; mkdir -p a/b; echo "st=$?"; ls a

#### mkdir -m sets the mode
mkdir -m 700 d; ls -ld d | cut -c1-10

#### mkdir of an existing directory fails
## skip-status
mkdir d; mkdir d 2>/dev/null; echo "failed=$?"

#### ln -s makes a symbolic link
touch t; ln -s t l; ls -l l | sed 's/.* l -> /l -> /'

#### ln makes a hard link
echo x > t; ln t h; cat h; ls -l t | awk '{print $2}'

#### ln -f replaces an existing target
echo one > a; echo two > b; ln -f a b; cat b

#### ln -sf replaces a symbolic link
touch a b; ln -s a l; ln -sf b l; ls -l l | sed 's/.* l -> /l -> /'

#### ln into a directory
mkdir d; touch a; ln -s ../a d; ls -l d/a | sed 's/.* -> //'

#### ln of an existing target without -f fails
## skip-status
touch a b; ln a b 2>/dev/null; echo "failed=$?"

#### touch makes a file and -c does not
touch new; touch -c none; ls

#### touch -r copies the time
touch -t 200001020304.05 ref; touch -r ref new; ls -l new | awk '{print $6, $7, $8}'

#### touch -t sets the time
touch -t 200102030405.06 f; ls -l f | awk '{print $6, $7, $8}'

#### touch -m and -a
touch -t 200001010000 f; touch -m -t 201003040000 f; ls -l f | awk '{print $6, $7, $8}'

#### mv renames a file
echo x > a; mv a b; ls; cat b

#### mv into a directory
mkdir d; touch a b; mv a b d; ls d

#### mv -f replaces
echo one > a; echo two > b; mv -f a b; cat b; ls

#### mv a directory
mkdir -p d/e; touch d/e/x; mv d n; ls n/e

#### cp -p keeps the time and mode
touch -t 200102030405 a; chmod 640 a; cp -p a b; ls -l b | awk '{print $1, $6, $7, $8}'

#### cp -R copies a tree
mkdir -p d/e; echo x > d/e/f; cp -R d n; cat n/e/f

#### cp -f replaces an unwritable target
echo one > a; echo two > b; chmod 444 b; cp -f a b; cat b

#### cp into a directory
mkdir d; echo x > a; echo y > b; cp a b d; ls d

#### chmod symbolic modes
touch f; chmod 600 f; chmod u+x,g+r,o=r f; ls -l f | cut -c1-10

#### chmod -R
mkdir -p d/e; touch d/e/f; chmod -R 700 d; ls -l d/e/f | cut -c1-10

#### chmod with a copy of a class
touch f; chmod 750 f; chmod g=u f; ls -l f | cut -c1-10

#### cat -u and -
echo a > f; echo b | cat -u f - f

#### cat of a missing file continues
## skip-status
echo a > f; cat nosuch f 2>/dev/null; echo "st=$?"

#### ls -di names the directory and its serial number
ls -di . | awk '{print ($1 > 0), $2}'

#### ls -i with files
touch a b; ls -i a b | awk '{print ($1 > 0), $2}'

#### ls -t sorts file operands newest first
touch -t 200001010000 old; touch new; ls -t old new

#### ls -rt sorts file operands oldest first
touch -t 200001010000 old; touch new; ls -rt old new

#### ls lists file operands before directories
mkdir d1 d2; touch d1/x d2/y f; ls d2 f d1

#### ls -L follows a symbolic link
touch t; ln -s t l; ls -L -l l | awk '{print substr($1, 1, 1)}'

#### ls -d with a directory operand
mkdir d; ls -d d
