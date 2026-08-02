package utf8

import (
	"strings"
	"testing"
	"unicode"

	"golang.org/x/text/language"
	"golang.org/x/text/search"
)

func TestIndexFold(t *testing.T) {
	tests := []struct {
		name             string
		haystack, needle string
		want             int
	}{
		{name: "empty", haystack: "anything", needle: "", want: 0},
		{name: "empty haystack", haystack: "", needle: "a", want: -1},
		{name: "ASCII", haystack: "Hello, WORLD!", needle: "world", want: 7},
		{name: "ASCII miss", haystack: "Hello", needle: "planet", want: -1},
		{name: "ASCII vector block", haystack: "0123456789abcdef0123456789ABCDEFindMe", needle: "findme", want: 31},
		{name: "ASCII vector miss", haystack: "0123456789abcdef0123456789abcdef", needle: "xyz", want: -1},
		{name: "sharp s expansion", haystack: "Straße", needle: "STRASSE", want: 0},
		{name: "inside expansion", haystack: "groß", needle: "s", want: 3},
		{name: "ligature", haystack: "ofﬁce", needle: "FFI", want: 1},
		{name: "Greek sigma", haystack: "κόσμος", needle: "ΣΜΟΣ", want: 4},
		{name: "Kelvin", haystack: "10K", needle: "k", want: 2},
		{name: "malformed equal", haystack: "a\xffb", needle: "\xffB", want: 1},
		{name: "malformed distinct from rune", haystack: "\xfc", needle: "ü", want: -1},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := IndexFold(test.haystack, test.needle); got != test.want {
				t.Fatalf("IndexFold(%q, %q) = %d, want %d", test.haystack, test.needle, got, test.want)
			}
		})
	}
}

func FuzzIndexFold(f *testing.F) {
	// x/text/search uses Unicode 6.2 collation tables. Keep the generated
	// strings to long-established letters for which IgnoreCase has the same
	// full-folding semantics as IndexFold, while still covering every SIMD
	// script classifier.
	alphabets := []string{
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_;",
		"ÀÁÂÃÄÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖØÙÚÛÜÝÞàáâãäæçèéêëìíîïðñòóôõöøùúûüýþ",
		"ĄĆČĐĘŁŃŐŘŚŠŤŹŻŽąćčđęłńőřśšťźżž",
		"АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЫЭЮЯабвгдежзийклмнопрстуфхцчшщыэюя",
		"ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψω",
		"ԱԲԳԴԵԶԷԸԹԺԻԼԽԾԿՀՁՂՃՄՅՆՇՈՉՊՋՌՍՎՏՐՑՒՓՔՕՖաբգդեզէըթժիլխծկհձղճմյնշոչպջռսվտրցւփքօֆ",
		"ĂÂĐÊÔƠƯÁÀẢÃẠẮẰẲẴẶẤẦẨẪẬÉÈẺẼẸẾỀỂỄỆÍÌỈĨỊÓÒỎÕỌỐỒỔỖỘỚỜỞỠỢÚÙỦŨỤỨỪỬỮỰÝỲỶỸỴăâđêôơưáàảãạắằẳẵặấầẩẫậéèẻẽẹếềểễệíìỉĩịóòỏõọốồổỗộớờởỡợúùủũụứừửữựýỳỷỹỵ",
		"აბგდევზთიკლმნოპჟრსტუფქღყშჩცძწჭხჯჰ",
	}

	seeds := []struct {
		script, control  byte
		haystack, needle string
	}{
		{0, 0, strings.Repeat("0123456789ABCDEF", 12) + "needle", "NEEDLE"},
		{1, 1, strings.Repeat("àéîöü", 28), "GARÇON"},
		{2, 2, strings.Repeat("ąćęłń", 28) + "żółw", "ŻÓŁW"},
		{3, 3, strings.Repeat("абвгде", 24), "ПРИВЕТ"},
		{4, 4, strings.Repeat("αβγδεζ", 24) + "κόσμος", "ΚΌΣΜΟΣ"},
		{5, 5, strings.Repeat("աբգդեզ", 24), "ՀԱՅԵՐ"},
		{6, 6, strings.Repeat("ắằẳẵặ", 24) + "tiếng", "TIẾNG"},
		{7, 7, strings.Repeat("აბგდევ", 24), "ქართული"},
	}
	for _, seed := range seeds {
		f.Add(seed.script, seed.control, []byte(seed.haystack), []byte(seed.needle))
	}

	reference := search.New(language.Und, search.IgnoreCase)
	f.Fuzz(func(t *testing.T, script, control byte, haystackData, needleData []byte) {
		alphabet := []rune(alphabets[int(script)%len(alphabets)])
		haystack := mapBytesToAlphabet(haystackData, alphabet, 512)
		needle := mapBytesToAlphabet(needleData, alphabet, 32)
		if needle == "" {
			needle = string(alphabet[int(script)%len(alphabet)])
		}
		const fuzzMinHaystackBytes = 192
		for len(haystack) < fuzzMinHaystackBytes {
			haystack += haystack
			if haystack == "" {
				haystack = strings.Repeat(string(alphabet[0]), fuzzMinHaystackBytes)
			}
		}
		if control&1 != 0 {
			haystackRunes := []rune(haystack)
			match := []rune(needle)
			if int(script)%len(alphabets) != len(alphabets)-1 {
				for i, r := range match {
					if (int(control)+i)&2 == 0 {
						match[i] = unicode.ToLower(r)
					} else {
						match[i] = unicode.ToUpper(r)
					}
				}
			}
			at := int(control>>1) % (len(haystackRunes) + 1)
			haystackRunes = append(haystackRunes, make([]rune, len(match))...)
			copy(haystackRunes[at+len(match):], haystackRunes[at:])
			copy(haystackRunes[at:], match)
			haystack = string(haystackRunes)
		}

		want, _ := reference.IndexString(haystack, needle)
		if fallback := indexFoldFallback(haystack, needle); fallback != want {
			t.Fatalf("indexFoldFallback(%q, %q) = %d, x/text/search = %d", haystack, needle, fallback, want)
		}
		if got := IndexFold(haystack, needle); got != want {
			t.Fatalf("IndexFold(%q, %q) = %d, x/text/search = %d", haystack, needle, got, want)
		}
	})
}

func mapBytesToAlphabet(data []byte, alphabet []rune, limit int) string {
	if len(data) > limit {
		data = data[:limit]
	}
	var b strings.Builder
	for _, value := range data {
		b.WriteRune(alphabet[int(value)%len(alphabet)])
	}
	return b.String()
}
