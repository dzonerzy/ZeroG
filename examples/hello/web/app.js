// App.greet is native code (main.c), called synchronously: no await.
const form = document.getElementById("form");
const name = document.getElementById("name");
const message = document.getElementById("message");

function greet() {
    message.textContent = App.greet(name.value);
    console.log(message.textContent);
}

form.addEventListener("submit", event => {
    event.preventDefault();
    greet();
});

greet();

// hello --shaped: the window is frameless and transparent; the card moves it.
if (location.hash === "#shaped") {
    document.documentElement.classList.add("shaped");
    document.getElementById("close").addEventListener("click", () => App.quit());
    addEventListener("keydown", event => {
        if (event.key === "Escape")
            App.quit();
    });
}

// The build's smoke test: quit once the greeting went through.
if (location.hash === "#smoke")
    requestAnimationFrame(() => App.quit());
