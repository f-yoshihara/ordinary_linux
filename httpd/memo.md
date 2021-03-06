# memo

##  プログラムの読み方

一般にプログラムを読む際にはデータ構造に注意を払う必要がある。
プログラムがどのようなデータ構造を使っていてそれにどのような制約をかけているのかがわかればコードはずっとわかりやすくなる。

プログラムはデータ構造をもちデータ構造には制約をかけることができる。

これは別の見方もできると思う。詳細に潜る前に上の階層から見ていくことが大切である。
read_request()の返り値はHTTPRequestとして取り扱われる。

HTTPRequestがどういうものなのかがあらかじめわかっていれば詳細な部分で右往左往する必要がなくなる。

## 正しいサボり

まず動くものを作る。
動くものとは何か？
それは正しく動くものではない。
動くだけである。
言い換えればただ、プログラムが最後まで実行されるだけである。
最後まで実行されることを確認したところでそれが正しく動くようになるようにする。

プログラムは同時にデバッガーでもある。
正しく動かないことはプログラムの方でも指摘してくれるので人間が動かす前から正しく動くように書く必要はない。

とりあえずは失敗するように実装する。

これをあらゆるレベルで徹底するべき、コンピュータは目的の処理を人間の代わりにやるだけではない。ある意味では実装も代わりにやってくれている。コンピュータにどれだけ実装をやらせることができるかが問題である。ある意味でそれはテストの回数に比例する。

もし、わざと失敗する実装をしながらプログラミングをしていて、そのつど処理をしながらデバッグを行なっているのであれば、ある意味では実装者はそれだけテストをしているのと同じことになる。

テストは単に面倒なものになっていくけれど、もし実装中にそれを行なっていれば、それはある意味ではプログラマーが考える作業をコンピューターにやらせているのと同じことなのではないだろうか？

なのでプログラマがやるべきこととはなにか？
つまり、コードを書くことではない。
むしろ考えることが重要なのでは？

この考える、とコーディングすることのバランスこそが、スキルなのではないだろうか？

議論が抽象的になりすぎているので具体例を出したい。

このようなスキルはどうやって身につけられるだろうか？
思いつく中で一番良いのはライブコーディングを見ることだと思う。
