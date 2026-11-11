// SPDX-License-Identifier: Apache-2.0
// Client-side filter over the sidebar navigation. No dependency.
(function () {
  var input = document.getElementById("filter");
  var items = Array.prototype.slice.call(document.querySelectorAll(".nav-item"));
  if (!input) return;
  input.addEventListener("input", function () {
    var q = input.value.trim().toLowerCase();
    items.forEach(function (item) {
      var title = (item.getAttribute("data-title") || "").toLowerCase();
      var match = !q || title.indexOf(q) !== -1 ||
                  item.textContent.toLowerCase().indexOf(q) !== -1;
      item.classList.toggle("hidden", !match);
    });
  });
})();