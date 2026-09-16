# Maintainer: OrnelasD-Rogers <https://github.com/OrnelasD-Rogers>
pkgname=acer-nitro-ec-dkms
pkgver=1.1.0
pkgrel=1
pkgdesc="Acer Nitro AN515/AN517 EC fan control driver (hwmon, DKMS)"
arch=('any')
url="https://github.com/OrnelasD-Rogers/acer-nitro-ec"
license=('GPL-2.0-only')
depends=('dkms')
# Use the canonical files beside this PKGBUILD for local package builds.
source=('acer-nitro-ec.c' 'Makefile' 'dkms.conf')
sha256sums=('SKIP' 'SKIP' 'SKIP')

package() {
	cd "$srcdir"

	local dest="$pkgdir/usr/src/acer-nitro-ec-$pkgver"
	install -dm755 "$dest"

	install -m644 acer-nitro-ec.c "$dest/"
	install -m644 Makefile         "$dest/"
	install -m644 dkms.conf        "$dest/"

	sed -i "s/@PKGVER@/$pkgver/" "$dest/dkms.conf"
}
