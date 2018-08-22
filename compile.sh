apt-get update 
apt-get install -y build-essential

"echo getting snapcast dependencies"
apt-get install -y libvorbisidec-dev libvorbis-dev libflac-dev alsa-utils libavahi-client-dev avahi-daemon

echo "cloning"
cd /home/volumio
git clone https://github.com/badaix/snapcast.git

echo "fetching submodules"
cd snapcast/externals
git submodule update --init --recursive

echo "Building"
cd ..
make 
