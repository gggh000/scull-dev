clear
echo "remove module..."
dkms remove  -m scull -v 1.0 --all

echo "copying module to /usr/src..."
rm -rf /usr/src/scull-1.0
mkdir /usr/src/scull-1.0
cd src
make clean
cd ..
cp -vr * /usr/src/scull-1.0/

echo "adding module..."
dkms add -m scull -v 1.0
echo "building module..."
dkms build  -m scull -v 1.0
echo "installing module..."
dkms install  -m scull -v 1.0

