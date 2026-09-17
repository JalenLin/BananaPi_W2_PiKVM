# 專案目標

* 該開發版自帶HDMI INPUT應充分利用
* 移植PiKVM到Banana Pi W2這張開發版上
* 目標做出sdcard image可以燒錄直接使用
* 盡量使用新的與mainline的程式碼

# 已知可能問題

* Banana Pi W2的HDMI Input在Linux下似乎沒有完整驅動
* Banana Pi W2的HDMI Input官方只有在Android下有示範

# 注意事項

* 製作過程中盡量保持系統乾淨，使用docker來處理各種相依

# 參考資料

* https://github.com/pikvm/pikvm
* https://github.com/BPI-SINOVOIP/BPI-W2-bsp
* https://docs.banana-pi.org/zh/BPI-W2/BananaPi_BPI-W2
* https://wiki.banana-pi.org/Banana_Pi_BPI-W2
